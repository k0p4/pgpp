// IT-REENTRY-001 through IT-REENTRY-012: calling the pool from its own executor
// threads, lifecycle operations racing each other, and connection hygiene
// between callers.
//
// User code runs on an executor thread in three places: exec/query callbacks,
// coroutine bodies after co_await, and transaction() work. The pool is a
// free-list of connections handed out as leases (REQ-PGPP-067); the sync API
// acquires on the calling thread and never touches the executor (REQ-PGPP-068),
// a callback runs after its lease was returned (REQ-PGPP-069), shutdown() from an
// executor thread is non-blocking (REQ-PGPP-070), initialize()/shutdown() are
// serialised (REQ-PGPP-071), and a connection is returned clean (REQ-PGPP-072).
//
// Every test that can hang runs its body on a detached thread under a watchdog,
// so a deadlock is a failed assertion, not a hung binary. A pool that deadlocked
// cannot be destroyed (shutdown() would wait for the stuck thread), so on failure
// the body's heap-allocated pool is abandoned on purpose. The data-race cases
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
    pool.prepareStatement({"r_one", "SELECT 1", {}});
    pool.prepareStatement({"r_pid", "SELECT pg_backend_pid()", {}});
}

// PREPARE validates the table, so the count statement is registered only once
// the table exists.
bool makeTable(PgppPool& pool)
{
    pool.execRawSync("DROP TABLE IF EXISTS pgpp_reentry");
    if (!pool.execRawSync("CREATE TABLE pgpp_reentry (name VARCHAR(64))")) {
        return false;
    }
    pool.prepareStatement({"r_count", "SELECT COUNT(*)::int4 FROM pgpp_reentry", {}});
    pool.prepareStatement({"r_names", "SELECT name::text FROM pgpp_reentry ORDER BY name", {}});
    return true;
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
// The callback runs on the executor thread after the statement's connection
// was returned to the pool, so the sync call can acquire that connection; it
// never waits for the executor.

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

// ── IT-REENTRY-002: the sync API never waits for the executor ───────────────
//
// Two callbacks sleep on both executor threads after their connections were
// returned. A sync call from the test thread must then complete at once: it
// acquires a connection directly, it does not queue behind the executor.

TEST(PoolReentrancy, SyncPathDoesNotUseExecutor)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(20s, [info]() -> std::string {
        auto* pool = new PgppPool;
        if (!pool->initialize(info, 2)) {
            return "initialize failed";
        }
        prepareHelpers(*pool);

        auto started = std::make_shared<std::atomic<int>>(0);
        auto release = std::make_shared<std::promise<void>>();
        auto gate    = std::make_shared<std::shared_future<void>>(release->get_future().share());
        for (int i = 0; i < 2; ++i) {
            pool->exec("r_one", [started, gate](std::optional<bool>) {
                started->fetch_add(1);
                gate->wait();                      // hold the executor thread, not a connection
            });
        }
        const auto untilStarted = std::chrono::steady_clock::now() + 5s;
        while (started->load() < 2 && std::chrono::steady_clock::now() < untilStarted) {
            std::this_thread::sleep_for(1ms);
        }
        if (started->load() < 2) {
            release->set_value();
            return "the two callbacks never started";
        }

        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = pool->execRawSync("SELECT 1");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        release->set_value();
        pool->shutdown();
        delete pool;
        if (!ok) {
            return "execRawSync failed";
        }
        if (elapsed > 500ms) {
            return "execRawSync took " + std::to_string(elapsed.count()) + " ms: it waited for the executor";
        }
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-002");
}

// ── IT-REENTRY-003: a pool call inside transaction() work is independent ────
//
// Inside `work` you use the connection you were given. A call through the pool
// API acquires a *second* connection: it is autocommit, independent of the
// transaction, and reports its own backend. Pinned so the rule stays visible.

TEST(PoolReentrancy, PoolCallInsideWorkIsIndependent)
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
            return "transaction must report false";
        }
        if (poolPid <= 0 || poolPid == txPid) {
            return "the pool call must run on its own connection (backend "
                 + std::to_string(poolPid) + " vs transaction's " + std::to_string(txPid) + ")";
        }
        // The transaction's own row is rolled back; the pool-API row was autocommitted.
        auto [ok, rows] = pool.querySync<std::tuple<std::string>>("r_names");
        if (!ok || rows.size() != 1 || std::get<0>(rows[0]) != "via_pool") {
            return "expected exactly the autocommitted 'via_pool' row, found " + std::to_string(rows.size());
        }

        dropTable(pool);
        pool.shutdown();
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-003");
}

// ── IT-REENTRY-004: a transaction from a callback, pool of one ──────────────
//
// transactionSync runs on the calling thread with its own lease, so it works
// from a callback even when the executor has a single thread. (Blocking on
// transaction().get() from a callback would wait for that same thread.)

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
            inner->set_value(pool->transactionSync([](PgppConnection& conn) {
                return conn.execRaw("INSERT INTO pgpp_reentry VALUES ('from_callback')");
            }));
        });
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "transactionSync inside the callback never returned (deadlock)";
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

// ── IT-REENTRY-005: transaction() inside transaction() work is independent ──
//
// The inner transaction acquires its own connection: it commits on its own and
// an outer rollback does not undo it. Standard pool behaviour, pinned.

TEST(PoolReentrancy, NestedTransactionIsIndependent)
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
        auto r = pool.transaction([&](PgppConnection& conn) {
            outerPid = PQbackendPID(conn.connection());
            conn.execRaw("INSERT INTO pgpp_reentry VALUES ('outer')");
            auto inner = pool.transaction([&](PgppConnection& innerConn) {
                innerPid = PQbackendPID(innerConn.connection());
                return innerConn.execRaw("INSERT INTO pgpp_reentry VALUES ('inner')");
            }).get();
            return inner.value_or(false) && false;   // inner committed on its own; outer aborts
        }).get();
        if (!r.has_value() || r.value()) {
            return "outer must report false";
        }
        if (innerPid <= 0 || innerPid == outerPid) {
            return "inner transaction must run on its own connection (backend "
                 + std::to_string(innerPid) + " vs outer's " + std::to_string(outerPid) + ")";
        }
        auto [ok, rows] = pool.querySync<std::tuple<std::string>>("r_names");
        if (!ok || rows.size() != 1 || std::get<0>(rows[0]) != "inner") {
            return "expected exactly the inner row to survive, found " + std::to_string(rows.size());
        }

        dropTable(pool);
        pool.shutdown();
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-005");
}

// ── IT-REENTRY-006: a callback issued from a callback runs later ────────────
//
// Pool of one. exec() from a callback queues like any other request; the inner
// callback runs on an executor thread after the outer callback returned, and
// it does run (the single executor thread is free again by then).

TEST(PoolReentrancy, CallbackIssuedFromCallbackRunsLater)
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
        const bool ranBefore = beforeFuture.get();
        if (innerFuture.wait_for(5s) != std::future_status::ready) {
            return "inner callback never fired";
        }
        pool->shutdown();
        delete pool;
        return ranBefore ? "inner callback ran before the outer exec() returned" : "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-006");
}

// ── IT-REENTRY-007: shutdown() from a callback ──────────────────────────────
//
// shutdown() on an executor thread cannot join itself: it stops the executor
// and returns; pending requests resolve (nullopt) as the threads drain and exit;
// a later shutdown()/destructor from another thread joins them. Run many
// times: the failure mode is a connection destroyed under a running thread or
// the pool freed while a thread is still finishing (ThreadSanitizer).

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
            pool->shutdown();   // from a non-executor thread: joins the exiting threads
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

// A pointer, not a reference: the pool outlives every coroutine in the storm.
FireAndForget stormCoroutine(PgppPool* db, std::shared_ptr<std::atomic<int>> completed)
{
    co_await coExec(*db, "r_one");
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
                            (void)pool->querySync<CountRow>("r_pid");
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
                            // Callback that itself uses the sync API (allowed: it does
                            // not use the executor) and fires another callback.
                            issued->fetch_add(1);
                            pool->exec("r_one", [=](std::optional<bool>) {
                                pool->execRawSync("SELECT 1");
                                issued->fetch_add(1);
                                pool->exec("r_one", [=](std::optional<bool>) {
                                    completed->fetch_add(1);
                                });
                                completed->fetch_add(1);
                            });
                            break;
                        case 4: {
                            // Transaction work uses the connection it was given.
                            issued->fetch_add(1);
                            auto f = pool->transaction([=](PgppConnection& conn) {
                                return conn.execRaw("INSERT INTO pgpp_reentry VALUES ('storm')")
                                    && conn.execRaw("INSERT INTO pgpp_reentry VALUES ('storm')");
                            });
                            if (f.valid()) {
                                f.get();
                            }
                            completed->fetch_add(1);
                            break;
                        }
                        default:
                            issued->fetch_add(1);
                            stormCoroutine(pool, completed);
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

// ── IT-REENTRY-011: a connection comes back clean ───────────────────────────
//
// Pool of one, so every call lands on the same connection. A caller that
// leaves a transaction open (raw BEGIN, never committed) or aborted (an error
// inside a transaction) must not affect the next caller: the pool rolls the
// stray transaction back when the connection is returned.

TEST(PoolReentrancy, ReleaseRollsBackStrayTransaction)
{
    auto info = getTestConnectionInfo();
    auto outcome = runUnderWatchdog(30s, [info]() -> std::string {
        PgppPool pool;
        if (!pool.initialize(info, 1)) {
            return "initialize failed";
        }
        prepareHelpers(pool);
        if (!makeTable(pool)) {
            return "table setup failed";
        }

        // Case 1: a stray BEGIN must not swallow the next caller's INSERT.
        pool.execRawSync("BEGIN");
        if (!pool.execRawSync("INSERT INTO pgpp_reentry VALUES ('after_stray_begin')")) {
            return "case 1: INSERT after a stray BEGIN failed";
        }
        pool.shutdown();
        if (!pool.initialize(info, 1)) {
            return "case 1: re-initialize failed";
        }
        prepareHelpers(pool);
        pool.prepareStatement({"r_count", "SELECT COUNT(*)::int4 FROM pgpp_reentry", {}});
        if (const int n = countRows(pool); n != 1) {
            return "case 1: the INSERT was lost inside another caller's transaction (rows: " + std::to_string(n) + ")";
        }

        // Case 2: an aborted transaction must not poison the connection.
        if (pool.execRawSync("BEGIN; SELECT no_such_column")) {
            return "case 2: the failing statement reported success";
        }
        if (!pool.execRawSync("SELECT 1")) {
            return "case 2: the connection stayed in the aborted transaction";
        }

        dropTable(pool);
        pool.shutdown();
        return "";
    });
    EXPECT_FINISHED(outcome, "IT-REENTRY-011");
}
