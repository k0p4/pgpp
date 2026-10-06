// UT-NOEXC-001 through UT-NOEXC-007: the library never throws (REQ-PGPP-061)
// No database required: an uninitialized pool exercises every request-building
// path up to enqueueRaw, which is where the failure value must come out.
//
// The throwing pieces are injected from the outside: a parameter type whose
// copy constructor throws (copying it into the request is library code), a
// callback that throws on the calling thread, and a transaction functor whose
// copy throws. Each call must return its failure value instead of propagating.

#include <pgpp/pgpp.h>
#include <pgpp/pgpp_coroutines.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

// If anything in this binary still reaches std::terminate, say why on stderr
// before aborting (a bare abort gives no clue on CI runners).
struct TerminateDiagnostics {
    TerminateDiagnostics()
    {
        std::set_terminate([] {
            std::fputs("[pgpp unit tests] std::terminate called", stderr);
            try {
                if (auto current = std::current_exception()) {
                    std::rethrow_exception(current);
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, " with exception: %s", e.what());
            } catch (...) {
                std::fputs(" with a non-std exception", stderr);
            }
            std::fputc('\n', stderr);
            std::fflush(stderr);
            std::abort();
        });
    }
};
const TerminateDiagnostics terminateDiagnostics;

// Looks like a query parameter (has .c_str()), but cannot be copied.
struct ThrowingArg {
    ThrowingArg() = default;
    ThrowingArg(const ThrowingArg&) { throw std::runtime_error("ThrowingArg copy"); }
    ThrowingArg& operator=(const ThrowingArg&) = delete;
    const char* c_str() const noexcept { return "x"; }
};

// A transaction body that cannot be moved into the request.
struct ThrowingWork {
    ThrowingWork() = default;
    ThrowingWork(const ThrowingWork&) { throw std::runtime_error("ThrowingWork copy"); }
    ThrowingWork(ThrowingWork&&) { throw std::runtime_error("ThrowingWork move"); }
    void operator()(PgppConnection&) const {}
};

} // namespace

// ── UT-NOEXC-001: the public API is declared noexcept ───────────────────────

TEST(NoExceptions, PublicApiIsNoexcept)
{
    PgppPool pool;
    PgppConnectionInfo info;
    Statement stmt;
    std::string s;
    static_assert(noexcept(pool.initialize(info, 1)));
    static_assert(noexcept(pool.shutdown()));
    static_assert(noexcept(pool.prepareStatement(stmt)));
    static_assert(noexcept(pool.execSync(s, s)));
    static_assert(noexcept(pool.querySync<std::tuple<int>>(s, s)));
    static_assert(noexcept(pool.execAsync(s, s)));
    static_assert(noexcept(pool.queryAsync<std::tuple<int>>(s, s)));
    static_assert(noexcept(pool.execRawSync(s)));
    static_assert(noexcept(pool.execRawAsync(s)));
    static_assert(noexcept(pool.transaction([](PgppConnection&) {})));
    static_assert(noexcept(pool.transaction([](PgppConnection&) { return false; })));
    static_assert(noexcept(pool.queuedRequests()));

    PgppConnection conn;
    static_assert(noexcept(conn.open(s)));
    static_assert(noexcept(conn.prepare(stmt)));
    static_assert(noexcept(conn.execRaw(s)));
    static_assert(noexcept(conn.lastError()));
    SUCCEED();
}

// ── UT-NOEXC-002: execAsync / queryAsync with an argument whose copy throws ──

TEST(NoExceptions, ExecAsyncArgumentCopyFailureYieldsNullopt)
{
    PgppPool pool;   // not initialized: the request is still built (and copied) before enqueueRaw
    ThrowingArg arg;

    std::future<std::optional<bool>> future;
    ASSERT_NO_THROW(future = pool.execAsync("stmt", arg));
    ASSERT_TRUE(future.valid());
    EXPECT_EQ(future.get(), std::nullopt);
}

TEST(NoExceptions, QueryAsyncArgumentCopyFailureYieldsNullopt)
{
    PgppPool pool;
    ThrowingArg arg;

    std::future<std::pair<std::optional<bool>, std::vector<std::tuple<int>>>> future;
    ASSERT_NO_THROW(future = pool.queryAsync<std::tuple<int>>("stmt", arg));
    ASSERT_TRUE(future.valid());
    auto [ok, rows] = future.get();
    EXPECT_EQ(ok, std::nullopt);
    EXPECT_TRUE(rows.empty());
}

// ── UT-NOEXC-003: the sync wrappers on the same failure ─────────────────────

TEST(NoExceptions, SyncArgumentCopyFailureReturnsFalse)
{
    PgppPool pool;
    ThrowingArg arg;

    bool execOk = true;
    ASSERT_NO_THROW(execOk = pool.execSync("stmt", arg));
    EXPECT_FALSE(execOk);

    std::pair<bool, std::vector<std::tuple<int>>> result { true, {} };
    ASSERT_NO_THROW(result = pool.querySync<std::tuple<int>>("stmt", arg));
    EXPECT_FALSE(result.first);
    EXPECT_TRUE(result.second.empty());
}

// ── UT-NOEXC-004: callbacks fire once with nullopt, even if they throw ──────

TEST(NoExceptions, CallbackFailuresAreContained)
{
    PgppPool pool;
    ThrowingArg arg;

    // Request cannot be built: the callback still fires exactly once, with nullopt.
    int fired = 0;
    std::optional<bool> seen = true;
    ASSERT_NO_THROW(pool.exec("stmt", [&](std::optional<bool> r) { ++fired; seen = r; }, arg));
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(seen, std::nullopt);

    // The callback itself throws on the calling thread: contained.
    ASSERT_NO_THROW(pool.exec("stmt", [](std::optional<bool>) { throw std::runtime_error("callback"); }));
    ASSERT_NO_THROW(pool.query<std::tuple<int>>("stmt",
        [](std::optional<bool>, std::vector<std::tuple<int>>) { throw std::runtime_error("callback"); }));
}

// ── UT-NOEXC-005: transaction with a work functor whose copy/move throws ────

TEST(NoExceptions, TransactionWorkCopyFailureYieldsNullopt)
{
    PgppPool pool;
    ThrowingWork work;

    std::future<std::optional<bool>> future;
    ASSERT_NO_THROW(future = pool.transaction(work));
    ASSERT_TRUE(future.valid());
    EXPECT_EQ(future.get(), std::nullopt);
}

// ── UT-NOEXC-006: coExec / coQuery with an argument whose copy throws ────────

TEST(NoExceptions, CoroutineArgumentCopyFailureCompletesWithNullopt)
{
    PgppPool pool;
    ThrowingArg arg;

    std::optional<bool> execResult = true;
    std::optional<bool> queryResult = true;
    bool finished = false;

    // Both awaitables must complete immediately with nullopt instead of
    // throwing into the coroutine (which FireAndForget would only log).
    auto run = [&]() -> FireAndForget {
        execResult = co_await coExec(pool, "stmt", arg);
        auto [ok, rows] = co_await coQuery<std::tuple<int>>(pool, "stmt", arg);
        queryResult = ok;
        finished = rows.empty();
    };
    ASSERT_NO_THROW(run());

    EXPECT_TRUE(finished);
    EXPECT_EQ(execResult, std::nullopt);
    EXPECT_EQ(queryResult, std::nullopt);
}
