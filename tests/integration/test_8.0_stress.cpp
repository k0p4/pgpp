// IT-STRESS-* : stress and edge-case tests covering findings from the
// 2026-04-18 pgpp review and the 2026-10-06 follow-up (startup race,
// lost wake-up on shutdown).
//
//   - pool exhaustion under sustained load
//   - connection reset mid-transaction
//   - callback-based queries in flight at shutdown
//   - concurrent prepareStatement from multiple threads
//   - coroutine exception propagation
//   - coroutine shutdown-while-suspended
//   - parameter edge cases (long strings, Unicode, embedded NULs)
//   - prepareStatement immediately after initialize (worker startup race)
//   - shutdown immediately after initialize (lost condition-variable wake-up)

#include "integration_fixture.h"
#include <pgpp/pgpp_coroutines.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>
#include <vector>

// ============================================================================
// Pool exhaustion under sustained load
//
// With pool size = N, firing M > N long-running queries must not deadlock.
// Queries queue up, workers process them in FIFO order, all complete.
// ============================================================================

TEST(PoolStress, ExhaustionUnderSustainedLoad)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    constexpr size_t kPoolSize = 2;
    constexpr size_t kQueries  = 6;                           // 3× pool
    ASSERT_TRUE(pool.initialize(info, kPoolSize));

    // Each query sleeps 250 ms server-side. With 2 workers and 6 queries the
    // total wall-clock is ~3 × 250 ms = 750 ms + epsilon. A 3 s budget is
    // comfortably above any reasonable CI jitter.
    auto start = std::chrono::steady_clock::now();

    std::vector<std::future<std::optional<bool>>> futures;
    for (size_t i = 0; i < kQueries; ++i)
        futures.push_back(pool.execRawAsync("SELECT pg_sleep(0.25)"));

    // While in-flight, queuedRequests + busyConnections must stay consistent.
    // (Can't demand an exact value — there's a race between fire and pickup —
    // but it must never exceed what we submitted.)
    const size_t submitted = kQueries;
    const size_t total     = pool.totalConnections();
    EXPECT_LE(pool.busyConnections(), total);
    EXPECT_LE(pool.busyConnections() + pool.queuedRequests(), submitted);

    size_t ok = 0;
    for (auto& f : futures) {
        auto r = f.get();
        if (r.has_value() && r.value()) ++ok;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(ok, kQueries)                             << "every query must complete";
    EXPECT_LT(elapsed.count(), 3000)                    << "no deadlock / starvation";
    EXPECT_GE(elapsed.count(), 500)                     << "queries actually queued";

    pool.shutdown();
}

// ============================================================================
// Connection reset mid-transaction
//
// pg_terminate_backend kills the backend serving the current transaction.
// The worker's libpq connection goes bad; the pool's `!conn->isOpen()` check
// must fire on the NEXT request and trigger PQreset. A follow-up query on the
// SAME pool must succeed — proving auto-recovery works.
// ============================================================================

TEST(PoolStress, ConnectionSurvivesBackendKill)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    // Pool size 1 forces the same connection to be reused for the follow-up
    // query, guaranteeing we exercise the reset path (otherwise the
    // follow-up could be round-robined to a healthy worker).
    ASSERT_TRUE(pool.initialize(info, 1));

    // Registered before the kill: the new server session knows nothing about
    // it, so the reconnect path must prepare it again (REQ-PGPP-043).
    pool.prepareStatement({"survive_sel", "SELECT 7", {}});
    using Row = std::tuple<int>;
    {
        auto [ok, rows] = pool.querySync<Row>("survive_sel");
        ASSERT_TRUE(ok);
        ASSERT_EQ(rows.size(), 1u);
    }

    // Transaction that deliberately kills its own backend. The COMMIT issued
    // by the transaction() helper will fail because the connection is gone
    // → result is `false` (failure), not nullopt.
    auto killFuture = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("SELECT pg_terminate_backend(pg_backend_pid())");
    });
    auto killResult = killFuture.get();
    EXPECT_TRUE(killResult.has_value())     << "transaction path must resolve (not hang)";
    EXPECT_FALSE(killResult.value_or(true)) << "transaction must report failure after self-kill";

    // Now prove the pool recovered: the very next request goes through the
    // same (reset) connection and must succeed, raw and prepared alike.
    EXPECT_TRUE(pool.execRawSync("SELECT 1")) << "pool must auto-reconnect after backend kill";
    auto [ok, rows] = pool.querySync<Row>("survive_sel");
    EXPECT_TRUE(ok) << "prepared statements must be prepared again on the new session";
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 7);

    pool.shutdown();
}

// ============================================================================
// Callback-based queries in flight at shutdown
//
// N callbacks are in flight when shutdown() is called. Each one must fire
// exactly once: either with a valid result (if the worker finished it before
// shutdown won) or with nullopt (drained queue). Dropping a callback on the
// floor would deadlock a caller waiting on its side-effects.
// ============================================================================

TEST(PoolStress, CallbackInFlightAtShutdown)
{
    auto info = getTestConnectionInfo();
    auto pool = std::make_unique<PgppPool>();
    ASSERT_TRUE(pool->initialize(info, 1));

    pool->prepareStatement({"cb_sleep", "SELECT pg_sleep(0.1)::text", {}});   // ::text: pg_sleep() returns void

    constexpr int kCallbacks = 20;
    std::atomic<int> fired { 0 };
    std::atomic<int> withValue { 0 };      // resolved via a worker
    std::atomic<int> withTrue  { 0 };      // ...and the statement actually ran
    std::atomic<int> drained   { 0 };      // resolved via shutdown drain

    // query<> so the result path is covered too. The ::text cast is required:
    // binary results (REQ-PGPP-062) read only textual columns into std::string,
    // and pg_sleep() is void.
    using Row = std::tuple<std::string>;
    for (int i = 0; i < kCallbacks; ++i) {
        pool->query<Row>("cb_sleep", [&](std::optional<bool> r, std::vector<Row>) {
            fired.fetch_add(1);
            if (r.has_value()) {
                withValue.fetch_add(1);
                if (r.value()) withTrue.fetch_add(1);
            } else {
                drained.fetch_add(1);
            }
        });
    }

    // Let the first statement complete, then shut down while the queue is
    // still full. shutdown() from a non-executor thread is synchronous
    // (REQ-PGPP-070): when it returns, every callback has fired.
    for (int i = 0; i < 500 && fired.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pool->shutdown();

    EXPECT_EQ(fired.load(), kCallbacks)
        << "every submitted callback must fire exactly once";
    EXPECT_GT(drained.load(), 0)
        << "some callbacks must come through the shutdown-drain path "
           "(test design — with pool size 1 + 20 queued, most get drained)";
    EXPECT_EQ(withValue.load() + drained.load(), kCallbacks);
    // A callback that reached an executor thread must have run the statement:
    // "cb_sleep" was registered right after initialize(), which used to be lost
    // by workers that had not started yet (see PrepareStatementRightAfterInitialize).
    // The one statement in flight when shutdown() arrived is cancelled
    // (REQ-PGPP-076) and reports false; with one connection that is at most one.
    EXPECT_GE(withTrue.load(), 1)
        << "callbacks served before shutdown must succeed, not fail with "
           "'prepared statement does not exist'";
    EXPECT_LE(withValue.load() - withTrue.load(), 1)
        << "only the statement cancelled by shutdown() may report false";
}

// ============================================================================
// prepareStatement() immediately after initialize()
//
// Workers used to read their initial statement version in the thread body. A
// prepareStatement() call that lands before a worker's first instruction was
// then absorbed into that initial read, so the worker never re-prepared and
// every exec/query of that statement failed with "does not exist". With one
// connection the request always goes to that worker, so this fails
// deterministically without the fix.
// ============================================================================

TEST(PoolStress, PrepareStatementRightAfterInitialize)
{
    auto info = getTestConnectionInfo();
    constexpr int kIterations = 20;

    for (int i = 0; i < kIterations; ++i) {
        PgppPool pool;
        ASSERT_TRUE(pool.initialize(info, 1));

        const std::string name = "late_prep_" + std::to_string(i);
        pool.prepareStatement({name, "SELECT 1", {}});

        auto [ok, rows] = pool.querySync<std::tuple<int>>(name);
        EXPECT_TRUE(ok) << "iteration " << i
                        << ": statement registered right after initialize() "
                           "was never prepared on the worker";
        pool.shutdown();
    }
}

// ============================================================================
// shutdown() immediately after initialize()
//
// stopWorkerThreads() used to set m_shuttingDown and notify without holding
// m_queueMutex. A worker that had just evaluated its wait predicate (false)
// but not yet blocked missed that notification and slept forever, so join()
// never returned. The window is a few instructions wide, so hammer it with
// many initialize/shutdown cycles. The loop runs on a detached thread so that
// a hang shows up as a failed timeout instead of a hung test binary.
// ============================================================================

TEST(PoolStress, ShutdownRightAfterInitializeDoesNotHang)
{
    auto info = getTestConnectionInfo();
    // One worker per pool, so its start-up coincides with shutdown(). The gap
    // between the two is swept over 0..127 us per cycle to land in the window
    // whatever the thread start-up latency is on this machine. Measured without
    // the fix: hangs within ~50-250 cycles on every run.
    static constexpr int    kCycles   = 800;
    static constexpr size_t kPoolSize = 1;

    auto done   = std::make_shared<std::promise<int>>();
    auto future = done->get_future();

    std::thread([info, done]() {
        for (int i = 0; i < kCycles; ++i) {
            PgppPool pool;
            if (!pool.initialize(info, kPoolSize)) { done->set_value(-1); return; }
            const auto until = std::chrono::steady_clock::now()
                             + std::chrono::microseconds(i % 128);
            while (std::chrono::steady_clock::now() < until) { /* spin */ }
            pool.shutdown();
        }
        done->set_value(kCycles);
    }).detach();

    ASSERT_EQ(future.wait_for(std::chrono::seconds(60)), std::future_status::ready)
        << "shutdown() hung: a worker missed the shutdown wake-up";
    EXPECT_EQ(future.get(), kCycles) << "pool.initialize() failed mid-loop";
}

// ============================================================================
// Concurrent prepareStatement from multiple threads
//
// Ten threads each register a distinct prepared statement. All must be
// usable from the pool afterwards (they are re-prepared on every worker's
// connection via the stmtVersion increment). No crashes, no duplicates.
// ============================================================================

TEST_F(PgppIntegrationTest, ConcurrentPrepareStatementFromThreads)
{
    constexpr int kThreads = 10;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([this, i]() {
            Statement s;
            s.statementName = "concur_prep_" + std::to_string(i);
            s.statement     = "SELECT $1::int4";
            s.variables     = {pg::INT4};
            pool.prepareStatement(s);
        });
    }
    for (auto& t : threads) t.join();

    // Every statement must be usable now. If prepareStatement had a race
    // (e.g. partial push to m_preparedStatements or a lost stmtVersion
    // bump), some of these queries would fail with "prepared statement does
    // not exist".
    for (int i = 0; i < kThreads; ++i) {
        using Row = std::tuple<int>;
        auto [ok, rows] = pool.querySync<Row>(
            "concur_prep_" + std::to_string(i), std::to_string(i * 7));
        EXPECT_TRUE(ok) << "concurrent-prepared statement #" << i << " must work";
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(std::get<0>(rows[0]), i * 7);
    }
}

// ============================================================================
// Coroutine exception propagation
//
// A FireAndForget that throws after co_await must have its exception caught
// and logged by promise_type::unhandled_exception. The process must stay
// alive and the pool must remain usable.
// ============================================================================

TEST_F(PgppIntegrationTest, CoroutineThrowAfterAwait)
{
    pool.prepareStatement({"coro_throw",
                           "INSERT INTO pgpp_test_table (name) VALUES ($1)",
                           {pg::VARCHAR}});

    std::atomic<bool> preThrow  { false };
    std::atomic<bool> postThrow { false };

    // The closure is a named local that outlives the coroutine (the test waits for it); see usage.md.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&]() -> FireAndForget {
        // Succeed first...
        co_await coExec(pool, "coro_throw", std::string("pre_throw"));
        preThrow.store(true);
        // ...then throw. promise_type::unhandled_exception catches it; the
        // coroutine is NOT re-entered after the throw, so postThrow stays
        // false — that's the assertion that distinguishes "exception was
        // swallowed" from "exception silently ignored".
        throw std::runtime_error("intentional coroutine throw");
        postThrow.store(true);                                // unreachable
    };
    coro();

    // Wait for the first co_await to finish AND the throw to propagate.
    for (int i = 0; i < 100 && !preThrow.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(preThrow.load())  << "first co_await must succeed";
    EXPECT_FALSE(postThrow.load()) << "statement after throw must be unreachable";

    // Pool must still be usable.
    EXPECT_TRUE(pool.execRawSync("SELECT 1"));
}

// ============================================================================
// Coroutine shutdown-while-suspended
//
// Launch a coroutine that co_awaits on a pool we immediately shut down
// (while the request is still queued, NOT yet picked up by a worker).
// Shutdown must drain the pending request with conn=nullptr — the awaitable
// yields nullopt, the coroutine completes cleanly, no dangling anything.
// ============================================================================

TEST(CoroutineStress, ShutdownWhileSuspended)
{
    auto info = getTestConnectionInfo();
    auto pool = std::make_unique<PgppPool>();
    ASSERT_TRUE(pool->initialize(info, 1));

    // Saturate the single worker with one slow query so the next one queues.
    auto slowHold = pool->execRawAsync("SELECT pg_sleep(0.3)");

    std::atomic<bool>   done { false };
    std::optional<bool> coResult;

    // The closure is a named local that outlives the coroutine (the test waits for it); see usage.md.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&]() -> FireAndForget {
        coResult = co_await coExec(*pool, "no_such_stmt", std::string("x"));
        done.store(true);
    };
    coro();

    // Shutdown before the slow query finishes — the coroutine's request
    // will be drained, not executed.
    pool->shutdown();

    // Slow future must also resolve (either completed before shutdown or
    // drained with nullopt). Test design only requires no-hang.
    auto status = slowHold.wait_for(std::chrono::seconds(3));
    EXPECT_NE(status, std::future_status::timeout);

    // Coroutine must have resumed (either via drain or worker).
    for (int i = 0; i < 100 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(done.load()) << "coroutine must resume after shutdown drain";

    // When drained, coResult is nullopt. When executed, it's false (no such
    // stmt). Either is acceptable; neither is "true".
    EXPECT_FALSE(coResult.value_or(false));
}

// ============================================================================
// Parameter edge cases
// ============================================================================

TEST_F(PgppIntegrationTest, ParamLongString)
{
    // 1 MB string parameter. Text-format protocol, but libpq supports
    // multi-megabyte parameters — only memory limits and bandwidth apply.
    //
    // The fixture's name column is VARCHAR(255) — too small for 1 MB.
    // Create a dedicated persistent (non-TEMP) table so every pool worker
    // sees it, then clean up.
    pool.execRawSync("CREATE TABLE IF NOT EXISTS pgpp_long_text (payload TEXT)");
    pool.execRawSync("TRUNCATE pgpp_long_text");

    const std::string big(1u * 1024u * 1024u, 'X');          // 1 MB of 'X'

    pool.prepareStatement({"long_ins",
                           "INSERT INTO pgpp_long_text (payload) VALUES ($1)",
                           {pg::TEXT}});
    pool.prepareStatement({"long_len",
                           "SELECT LENGTH(payload) FROM pgpp_long_text",
                           {}});

    EXPECT_TRUE(pool.execSync("long_ins", big));

    using Row = std::tuple<int64_t>;
    auto [ok, rows] = pool.querySync<Row>("long_len");
    ASSERT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), static_cast<int64_t>(big.size()));

    pool.execRawSync("DROP TABLE pgpp_long_text");
}

TEST_F(PgppIntegrationTest, ParamUnicode)
{
    // Mixed Cyrillic / Chinese / emoji to exercise multi-byte UTF-8.
    // Plain char string literal — C++20 changed u8 to char8_t, which
    // doesn't implicitly convert to std::string.
    const std::string utf8 =
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82 "           // "Привіт "
        "\xE4\xB8\x96\xE7\x95\x8C "                                     // "世界 "
        "\xF0\x9F\x8C\x8D \xE2\x9C\x94";                                // "🌍 ✔"

    pool.prepareStatement({"utf_ins",
                           "INSERT INTO pgpp_test_table (name) VALUES ($1)",
                           {pg::VARCHAR}});
    pool.prepareStatement({"utf_sel",
                           "SELECT name FROM pgpp_test_table WHERE name = $1",
                           {pg::VARCHAR}});

    EXPECT_TRUE(pool.execSync("utf_ins", utf8));

    using Row = std::tuple<std::string>;
    auto [ok, rows] = pool.querySync<Row>("utf_sel", utf8);
    ASSERT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), utf8);
}

TEST_F(PgppIntegrationTest, ParamEmptyString)
{
    // Distinct from NULL — empty string must be stored and retrievable as "".
    pool.prepareStatement({"empty_ins",
                           "INSERT INTO pgpp_test_table (name) VALUES ($1)",
                           {pg::VARCHAR}});
    pool.prepareStatement({"empty_sel",
                           "SELECT name FROM pgpp_test_table WHERE name = $1",
                           {pg::VARCHAR}});

    EXPECT_TRUE(pool.execSync("empty_ins", std::string("")));

    using Row = std::tuple<std::string>;
    auto [ok, rows] = pool.querySync<Row>("empty_sel", std::string(""));
    ASSERT_TRUE(ok);
    ASSERT_GE(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), "");
}

// Text-format parameters are NUL-terminated C strings, and PostgreSQL text
// cannot hold a NUL anyway: the bytes after an embedded NUL would be silently
// dropped (CWE-158). Such a parameter is rejected instead, nothing is sent
// (REQ-PGPP-074).
TEST_F(PgppIntegrationTest, ParamEmbeddedNulIsRejected)
{
    // "hello\0world" — 11 bytes in the std::string, but .c_str() yields 5.
    std::string withNul("hello");
    withNul.push_back('\0');
    withNul += "world";
    ASSERT_EQ(withNul.size(), 11u);

    pool.prepareStatement({"nul_ins",
                           "INSERT INTO pgpp_test_table (name) VALUES ($1)",
                           {pg::VARCHAR}});
    pool.prepareStatement({"nul_sel",
                           "SELECT name FROM pgpp_test_table WHERE name = $1",
                           {pg::VARCHAR}});
    pool.prepareStatement({"nul_cnt",
                           "SELECT COUNT(*)::int4 FROM pgpp_test_table",
                           {}});

    EXPECT_FALSE(pool.execSync("nul_ins", withNul)) << "a parameter with an embedded NUL must be refused";

    using Row = std::tuple<std::string>;
    auto [selOk, selRows] = pool.querySync<Row>("nul_sel", withNul);
    EXPECT_FALSE(selOk) << "the result overload refuses it too";
    EXPECT_TRUE(selRows.empty());

    auto [cntOk, cntRows] = pool.querySync<std::tuple<int>>("nul_cnt");
    ASSERT_TRUE(cntOk);
    ASSERT_EQ(cntRows.size(), 1u);
    EXPECT_EQ(std::get<0>(cntRows[0]), 0) << "nothing was inserted, truncated or otherwise";

    // The NUL-free prefix is an ordinary parameter.
    EXPECT_TRUE(pool.execSync("nul_ins", std::string("hello")));
}
