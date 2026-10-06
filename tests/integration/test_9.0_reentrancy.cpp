// IT-REENTRY-001 through IT-REENTRY-010: calling the pool from its own worker
// threads, and lifecycle operations racing each other.
//
// User code runs on a worker in three places: exec/query callbacks, coroutine
// bodies after co_await, and transaction() work. From there the pool must still
// be usable: a request issued from a worker executes inline on that worker's
// connection (REQ-PGPP-067), a transaction() inside transaction() work becomes a
// savepoint (REQ-PGPP-068), shutdown() from a worker defers its teardown to that
// worker (REQ-PGPP-069), and initialize()/shutdown()/stats never race each other
// (REQ-PGPP-070).
//
// Every test that can hang runs its body on a detached thread under a watchdog,
// so a deadlock is a failed assertion, not a hung binary. A pool that deadlocked
// cannot be destroyed (shutdown() would join the stuck worker), so on failure the
// body's heap-allocated pool is abandoned on purpose. The data-race cases
// (IT-REENTRY-007..009) are only visible under ThreadSanitizer; the suite is run
// under it in CI.

#include "integration_fixture.h"
#include <pgpp/pgpp_coroutines.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using CountRow = std::tuple<int>;

// Runs body() on a detached thread. body returns "" on success or a failure
// description. nullopt means the body did not finish within `limit`.
template<typename Body>
std::optional<std::string> runUnderWatchdog(std::chrono::seconds limit, Body body)
{
    auto done   = std::make_shared<std::promise<std::string>>();
    auto future = done->get_future();
    std::thread([done, body = std::move(body)]() mutable {
        done->set_value(body());
    }).detach();
    if (future.wait_for(limit) != std::future_status::ready) {
        return std::nullopt;
    }
    return future.get();
}

#define EXPECT_FINISHED(outcome, what)                                              \
    ASSERT_TRUE((outcome).has_value()) << what << ": did not finish (deadlock)";    \
    EXPECT_TRUE((outcome)->empty()) << what << ": " << *(outcome)

void prepareHelpers(PgppPool& pool)
{
    pool.prepareStatement({"r_one",   "SELECT 1", {}});
    pool.prepareStatement({"r_pid",   "SELECT pg_backend_pid()", {}});
    pool.prepareStatement({"r_count", "SELECT COUNT(*)::int4 FROM pgpp_reentry", {}});
}

bool makeTable(PgppPool& pool)
{
    pool.execRawSync("DROP TABLE IF EXISTS pgpp_reentry");
    return pool.execRawSync("CREATE TABLE pgpp_reentry (name VARCHAR(64))");
}

void dropTable(PgppPool& pool)
{
    pool.execRawSync("DROP TABLE IF EXISTS pgpp_reentry");
}

int countRows(PgppPool& pool)
{
    auto [ok, rows] = pool.querySync<CountRow>("r_count");
    if (ok && rows.size() == 1) {
        return std::get<0>(rows[0]);
    } else {
        return -1;
    }
}

int backendPidViaPool(PgppPool& pool)
{
    auto [ok, rows] = pool.querySync<CountRow>("r_pid");
    if (ok && rows.size() == 1) {
        return std::get<0>(rows[0]);
    } else {
        return -1;
    }
}

} // namespace

// ── IT-REENTRY-001: sync call from a callback, pool of one ──────────────────
//
// The callback runs on the pool's only worker. A blocking call from there must
// not wait for "a free worker" (there is none): it runs on this connection.

TEST(PoolReentrancy, SyncCallFromCallbackRunsInline)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(15s, [info]() -> std::string {
        auto* pool = new PgppPool;   // abandoned on deadlock, see file header
        if (!pool->initialize(info, 1)) {
            return "initialize failed";
        }
        prepareHelpers(*pool);

        auto inner = std::make_shared<std::promise<bool>>();
        auto innerFuture = inner->get_future();
        pool->exec("r_one", [pool, inner](std::optional<bool>) {
            // Not gated on the outer result: exec on a SELECT reports failure
            // until P8 lands, and that is not what this test is about.
            inner->set_value(pool->execRawSync("SELECT 1"));
        });
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "execRawSync inside the callback never returned (deadlock)";
        }
        const bool ok = innerFuture.get();
        pool->shutdown();
        delete pool;
        return ok ? "" : "execRawSync inside the callback returned false";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-001");
}

// ── IT-REENTRY-002: future.get() from a callback, pool of one ───────────────

TEST(PoolReentrancy, FutureGetFromCallbackRunsInline)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(15s, [info]() -> std::string {
        auto* pool = new PgppPool;
        if (!pool->initialize(info, 1)) {
            return "initialize failed";
        }
        prepareHelpers(*pool);

        auto inner = std::make_shared<std::promise<bool>>();
        auto innerFuture = inner->get_future();
        pool->exec("r_one", [pool, inner](std::optional<bool>) {
            auto f = pool->execRawAsync("SELECT 1");
            inner->set_value(f.valid() && f.get().value_or(false));
        });
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "future.get() inside the callback never returned (deadlock)";
        }
        const bool ok = innerFuture.get();
        pool->shutdown();
        delete pool;
        return ok ? "" : "execRawAsync inside the callback resolved false";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-002");
}

// ── IT-REENTRY-003: a pool call inside transaction() work stays inside ──────
//
// Pool of two, so there is a second worker to escape to. A statement issued
// through the pool from inside `work` must run on the transaction's own
// connection: it is rolled back with it, and committed with it.

TEST(PoolReentrancy, SyncCallInsideTransactionStaysInside)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(30s, [info]() -> std::string {
        PgppPool pool;
        if (!pool.initialize(info, 2)) {
            return "initialize failed";
        }
        prepareHelpers(pool);
        if (!makeTable(pool)) {
            return "table setup failed";
        }

        // Case 1: abort. Nothing may survive, including the row inserted
        // through the pool API, and that insert must have used the same backend.
        int txPid = 0;
        int poolPid = -1;
        auto r1 = pool.transaction([&](PgppConnection& conn) {
            txPid = PQbackendPID(conn.connection());
            conn.execRaw("INSERT INTO pgpp_reentry VALUES ('via_conn')");
            pool.execRawSync("INSERT INTO pgpp_reentry VALUES ('via_pool')");
            poolPid = backendPidViaPool(pool);
            return false;
        }).get();
        if (!r1.has_value() || r1.value()) {
            return "case 1: transaction must report false";
        }
        if (poolPid != txPid) {
            return "case 1: the pool call ran on backend " + std::to_string(poolPid)
                 + " instead of the transaction's backend " + std::to_string(txPid);
        }
        if (const int n = countRows(pool); n != 0) {
            return "case 1: " + std::to_string(n) + " row(s) survived the rollback";
        }

        // Case 2: commit. Both rows, atomically.
        auto r2 = pool.transaction([&](PgppConnection& conn) {
            return conn.execRaw("INSERT INTO pgpp_reentry VALUES ('via_conn')")
                && pool.execRawSync("INSERT INTO pgpp_reentry VALUES ('via_pool')");
        }).get();
        if (!r2.value_or(false)) {
            return "case 2: transaction must commit";
        }
        if (const int n = countRows(pool); n != 2) {
            return "case 2: expected 2 rows, found " + std::to_string(n);
        }

        dropTable(pool);
        pool.shutdown();
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-003");
}

// ── IT-REENTRY-004: transaction() from a callback, pool of one ──────────────

TEST(PoolReentrancy, TransactionFromCallbackCommits)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(15s, [info]() -> std::string {
        auto* pool = new PgppPool;
        if (!pool->initialize(info, 1)) {
            return "initialize failed";
        }
        prepareHelpers(*pool);
        if (!makeTable(*pool)) {
            return "table setup failed";
        }

        auto inner = std::make_shared<std::promise<bool>>();
        auto innerFuture = inner->get_future();
        pool->exec("r_one", [pool, inner](std::optional<bool>) {
            auto f = pool->transaction([](PgppConnection& conn) {
                return conn.execRaw("INSERT INTO pgpp_reentry VALUES ('from_callback')");
            });
            inner->set_value(f.valid() && f.get().value_or(false));
        });
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "transaction() inside the callback never resolved (deadlock)";
        }
        if (!innerFuture.get()) {
            return "transaction() inside the callback did not commit";
        }
        const int n = countRows(*pool);
        dropTable(*pool);
        pool->shutdown();
        delete pool;
        return n == 1 ? "" : "expected the committed row, found " + std::to_string(n);
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-004");
}

// ── IT-REENTRY-005: transaction() inside transaction() work is a savepoint ──
//
// The inner transaction runs on the outer's connection: its rollback undoes
// only its own statements, its commit is still undone by an outer rollback.

TEST(PoolReentrancy, NestedTransactionUsesSavepoint)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(30s, [info]() -> std::string {
        PgppPool pool;
        if (!pool.initialize(info, 2)) {
            return "initialize failed";
        }
        prepareHelpers(pool);
        if (!makeTable(pool)) {
            return "table setup failed";
        }

        int outerPid = 0;
        int innerPid = -1;
        auto nested = [&](bool innerResult, bool outerResult) {
            return pool.transaction([&, innerResult, outerResult](PgppConnection& conn) {
                outerPid = PQbackendPID(conn.connection());
                conn.execRaw("INSERT INTO pgpp_reentry VALUES ('outer')");
                auto inner = pool.transaction([&, innerResult](PgppConnection& innerConn) {
                    innerPid = PQbackendPID(innerConn.connection());
                    innerConn.execRaw("INSERT INTO pgpp_reentry VALUES ('inner')");
                    return innerResult;
                }).get();
                if (inner.value_or(!innerResult) != innerResult) {
                    return !outerResult;   // force a visible failure below
                }
                return outerResult;
            }).get();
        };

        // Case 1: inner aborts, outer commits → only 'outer'.
        auto r = nested(false, true);
        if (!r.value_or(false)) {
            return "case 1: outer must commit";
        }
        if (innerPid != outerPid) {
            return "case 1: inner transaction ran on backend " + std::to_string(innerPid)
                 + " instead of the outer's " + std::to_string(outerPid);
        }
        if (const int n = countRows(pool); n != 1) {
            return "case 1: expected 1 row, found " + std::to_string(n);
        }
        pool.execRawSync("DELETE FROM pgpp_reentry");

        // Case 2: both commit → both rows.
        r = nested(true, true);
        if (!r.value_or(false)) {
            return "case 2: outer must commit";
        }
        if (const int n = countRows(pool); n != 2) {
            return "case 2: expected 2 rows, found " + std::to_string(n);
        }
        pool.execRawSync("DELETE FROM pgpp_reentry");

        // Case 3: inner commits, outer aborts → nothing (the inner is nested).
        r = nested(true, false);
        if (!r.has_value() || r.value()) {
            return "case 3: outer must report false";
        }
        if (const int n = countRows(pool); n != 0) {
            return "case 3: " + std::to_string(n) + " row(s) survived the outer rollback";
        }

        dropTable(pool);
        pool.shutdown();
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-005");
}

// ── IT-REENTRY-006: a callback issued from a callback fires inline ──────────
//
// Pool of one. From a worker, exec() executes right away on that connection,
// so the inner callback has already run when the outer exec() returns.

TEST(PoolReentrancy, CallbackIssuedFromCallbackFiresInline)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(15s, [info]() -> std::string {
        auto* pool = new PgppPool;
        if (!pool->initialize(info, 1)) {
            return "initialize failed";
        }
        prepareHelpers(*pool);

        auto innerRan        = std::make_shared<std::atomic<bool>>(false);
        auto ranBeforeReturn = std::make_shared<std::promise<bool>>();
        auto innerDone       = std::make_shared<std::promise<void>>();
        auto beforeFuture    = ranBeforeReturn->get_future();
        auto innerFuture     = innerDone->get_future();

        pool->exec("r_one", [=](std::optional<bool>) {
            pool->exec("r_one", [=](std::optional<bool>) {
                innerRan->store(true);
                innerDone->set_value();
            });
            ranBeforeReturn->set_value(innerRan->load());
        });

        if (beforeFuture.wait_for(5s) != std::future_status::ready) {
            return "outer callback never finished";
        }
        const bool inline_ = beforeFuture.get();
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "inner callback never fired";
        }
        pool->shutdown();
        delete pool;
        return inline_ ? "" : "inner callback fired after the outer exec() returned (queued, not inline)";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-006");
}

// ── IT-REENTRY-007: shutdown() from a callback completes the teardown ───────
//
// shutdown() on a worker cannot join itself; it must return, finish the
// teardown after the callback, and a later shutdown()/destructor from another
// thread must wait for that. Pending requests resolve (nullopt). Run many
// times: the failure mode is a connection destroyed under the worker's feet
// or the pool freed while the worker is still finishing (ThreadSanitizer).

TEST(PoolReentrancy, ShutdownFromCallbackCompletesTeardown)
{
    auto info = getTestConnectionInfo();
    static constexpr int kCycles = 50;
    auto outcome = runUnderWatchdog(90s, [info]() -> std::string {
        for (int cycle = 0; cycle < kCycles; ++cycle) {
            auto* pool = new PgppPool;
            if (!pool->initialize(info, 2)) {
                return "initialize failed";
            }
            prepareHelpers(*pool);

            std::vector<std::future<std::optional<bool>>> pending;
            for (int i = 0; i < 4; ++i) {
                pending.push_back(pool->execRawAsync("SELECT pg_sleep(0.02)"));
            }
            auto cbDone = std::make_shared<std::promise<void>>();
            auto cbFuture = cbDone->get_future();
            pool->exec("r_one", [pool, cbDone](std::optional<bool>) {
                pool->shutdown();
                cbDone->set_value();
            });
            if (cbFuture.wait_for(5s) != std::future_status::ready) {
                return "cycle " + std::to_string(cycle) + ": callback never finished";
            }
            if (pool->isInitialized()) {
                return "cycle " + std::to_string(cycle) + ": still initialized after shutdown() from a callback";
            }
            for (auto& f : pending) {
                if (f.wait_for(5s) != std::future_status::ready) {
                    return "cycle " + std::to_string(cycle) + ": a pending request was never resolved";
                }
            }
            pool->shutdown();   // from a non-worker thread: waits for the deferred teardown
            delete pool;
        }
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-007");
}

// ── IT-REENTRY-008: statistics during initialize/shutdown are race-free ─────
//
// No assertion beyond "finishes": the detector is ThreadSanitizer.

TEST(PoolReentrancy, StatsDuringShutdownAreRaceFree)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    std::atomic<bool> stop { false };
    std::atomic<size_t> sink { 0 };
    std::thread reader([&]() {
        while (!stop.load()) {
            sink.fetch_add(pool.totalConnections() + pool.freeConnections()
                         + pool.busyConnections() + pool.queuedRequests(),
                           std::memory_order_relaxed);
        }
    });
    for (int cycle = 0; cycle < 30; ++cycle) {
        ASSERT_TRUE(pool.initialize(info, 2));
        pool.execRawSync("SELECT 1");
        pool.shutdown();
    }
    stop = true;
    reader.join();
    EXPECT_FALSE(pool.isInitialized());
}

// ── IT-REENTRY-009: initialize() and shutdown() from two threads ────────────
//
// Either call may win each round; the pool must end up fully initialized or
// fully shut down, never half of each. Detector: ThreadSanitizer, or a crash.

TEST(PoolReentrancy, ConcurrentInitializeShutdown)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    static constexpr int kRounds = 40;
    std::thread starter([&]() {
        for (int i = 0; i < kRounds; ++i) {
            pool.initialize(info, 2);
        }
    });
    std::thread stopper([&]() {
        for (int i = 0; i < kRounds; ++i) {
            pool.shutdown();
        }
    });
    starter.join();
    stopper.join();

    if (pool.isInitialized()) {
        EXPECT_EQ(pool.totalConnections(), 2u);
        EXPECT_TRUE(pool.execRawSync("SELECT 1"));
    } else {
        EXPECT_EQ(pool.totalConnections(), 0u);
    }
    pool.shutdown();
    EXPECT_FALSE(pool.isInitialized());
}

// ── IT-REENTRY-010: mixed API storm with re-entrant callbacks ───────────────
//
// Client threads use every API; callbacks and transaction work issue further
// requests; shutdown() lands mid-storm. Every request must complete, exactly
// once, at pool sizes 1, 2 and 8.

namespace {

FireAndForget stormCoroutine(PgppPool& db, std::shared_ptr<std::atomic<int>> completed)
{
    co_await coExec(db, "r_one");
    completed->fetch_add(1);
}

} // namespace

TEST(PoolReentrancy, MixedApiStorm)
{
    auto info = getTestConnectionInfo();
    for (const size_t poolSize : { size_t { 1 }, size_t { 2 }, size_t { 8 } }) {
        auto outcome = runUnderWatchdog(90s, [info, poolSize]() -> std::string {
            static constexpr int kClients    = 6;
            static constexpr int kIterations = 20;
            auto* pool = new PgppPool;
            if (!pool->initialize(info, poolSize)) {
                return "initialize failed";
            }
            prepareHelpers(*pool);
            if (!makeTable(*pool)) {
                return "table setup failed";
            }

            auto issued    = std::make_shared<std::atomic<int>>(0);
            auto completed = std::make_shared<std::atomic<int>>(0);

            std::vector<std::thread> clients;
            for (int c = 0; c < kClients; ++c) {
                clients.emplace_back([=]() {
                    for (int i = 0; i < kIterations; ++i) {
                        switch ((c + i) % 6) {
                        case 0:
                            issued->fetch_add(1);
                            pool->execSync("r_one");
                            completed->fetch_add(1);
                            break;
                        case 1:
                            issued->fetch_add(1);
                            pool->querySync<CountRow>("r_pid");
                            completed->fetch_add(1);
                            break;
                        case 2: {
                            issued->fetch_add(1);
                            auto f = pool->execAsync("r_one");
                            if (f.valid()) {
                                f.get();
                            }
                            completed->fetch_add(1);
                            break;
                        }
                        case 3:
                            // Callback that itself uses the sync and the future API.
                            issued->fetch_add(1);
                            pool->exec("r_one", [=](std::optional<bool>) {
                                pool->execRawSync("SELECT 1");
                                auto f = pool->execRawAsync("SELECT 1");
                                if (f.valid()) {
                                    f.get();
                                }
                                completed->fetch_add(1);
                            });
                            break;
                        case 4: {
                            // Transaction whose work goes through the pool API.
                            issued->fetch_add(1);
                            auto f = pool->transaction([=](PgppConnection& conn) {
                                return conn.execRaw("INSERT INTO pgpp_reentry VALUES ('storm')")
                                    && pool->execRawSync("INSERT INTO pgpp_reentry VALUES ('storm')");
                            });
                            if (f.valid()) {
                                f.get();
                            }
                            completed->fetch_add(1);
                            break;
                        }
                        default:
                            issued->fetch_add(1);
                            stormCoroutine(*pool, completed);
                            break;
                        }
                    }
                });
            }

            std::this_thread::sleep_for(300ms);
            pool->shutdown();              // mid-storm: remaining calls fail fast but complete
            for (auto& t : clients) {
                t.join();
            }
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (completed->load() < issued->load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
            const int left = issued->load() - completed->load();
            delete pool;
            if (left != 0) {
                return "pool size " + std::to_string(poolSize) + ": " + std::to_string(left)
                     + " request(s) never completed";
            }
            return "";
        });
        EXPECT_FINISHED(outcome, "IT-REENTRY-010 (pool size " + std::to_string(poolSize) + ")");
    }

    // Table cleanup on a fresh pool: the storm's pool may have been abandoned.
    PgppPool cleanup;
    if (cleanup.initialize(info, 1)) {
        dropTable(cleanup);
        cleanup.shutdown();
    }
}
