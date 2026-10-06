// IT-POOL-001 through IT-POOL-010: Pool operations tests

#include "integration_fixture.h"
#include <thread>
#include <atomic>
#include <vector>
#include <future>
#include <chrono>
#include <algorithm>
#include <optional>

namespace {

// Polls `ready` every 10 ms for up to 5 s.
template<typename Ready>
bool waitUntil(Ready ready)
{
    for (int i = 0; i < 500 && !ready(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return ready();
}

} // namespace

// ── IT-POOL-001: initialize with 2 connections ──────────────────────────────

TEST_F(PgppIntegrationTest, PoolInitializeWithConnections)
{
    EXPECT_EQ(pool.totalConnections(), 2u);
    EXPECT_TRUE(pool.isInitialized());
}

// Verify double-initialize on an already-running pool returns true (idempotent)
TEST_F(PgppIntegrationTest, DoubleInitializeReturnsTrueWhenAlreadyRunning)
{
    // pool is already initialized by fixture's SetUp
    ASSERT_TRUE(pool.isInitialized());

    // Second call hits the atomic guard: m_initialized.exchange(true) returns true → return true
    EXPECT_TRUE(pool.initialize(connInfo, 2));
    EXPECT_TRUE(pool.isInitialized());
    // Pool should still work
    EXPECT_TRUE(pool.execRawSync("SELECT 1"));
}

// ── IT-POOL-002: execSync INSERT ────────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolExecSyncInsert)
{
    pool.prepareStatement({"pool_ins", "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)", {pg::VARCHAR, pg::INT4}});
    EXPECT_TRUE(pool.execSync("pool_ins", std::string("alice"), std::string("50")));
}

// ── IT-POOL-003: querySync SELECT ───────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolQuerySyncSelect)
{
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('bob', 99)");
    pool.prepareStatement({"pool_sel", "SELECT name, score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    using Row = std::tuple<std::string, int>;
    auto [ok, rows] = pool.querySync<Row>("pool_sel", std::string("bob"));
    EXPECT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), "bob");
    EXPECT_EQ(std::get<1>(rows[0]), 99);
}

// ── IT-POOL-004: execAsync future ───────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolExecAsyncFuture)
{
    pool.prepareStatement({"async_ins", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});
    auto future = pool.execAsync("async_ins", std::string("charlie"));
    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());
}

// ── IT-POOL-005: queryAsync future ──────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolQueryAsyncFuture)
{
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('dave', 77)");
    pool.prepareStatement({"async_sel", "SELECT name, score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    using Row = std::tuple<std::string, int>;
    auto future = pool.queryAsync<Row>("async_sel", std::string("dave"));
    auto [ok, rows] = future.get();
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok.value());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), "dave");
}

// ── IT-POOL-006: callback fires on worker thread ────────────────────────────

TEST_F(PgppIntegrationTest, PoolCallbackOnWorkerThread)
{
    pool.prepareStatement({"cb_ins", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});

    std::atomic<std::thread::id> callbackThread;
    std::promise<void> done;

    pool.exec("cb_ins",
        [&callbackThread, &done](std::optional<bool> ok) {
            callbackThread.store(std::this_thread::get_id());
            done.set_value();
        },
        std::string("eve"));

    done.get_future().wait();
    EXPECT_NE(callbackThread.load(), std::this_thread::get_id());
}

// ── IT-POOL-007: concurrent queries from N threads ──────────────────────────

TEST_F(PgppIntegrationTest, PoolConcurrentQueries)
{
    pool.prepareStatement({"conc_ins", "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)", {pg::VARCHAR, pg::INT4}});

    constexpr int numThreads = 10;
    constexpr int queriesPerThread = 20;
    std::atomic<int> successCount { 0 };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, t, &successCount]() {
            for (int q = 0; q < queriesPerThread; ++q) {
                std::string name = "t" + std::to_string(t) + "_q" + std::to_string(q);
                if (pool.execSync("conc_ins", name, std::to_string(q)))
                    successCount.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(successCount.load(), numThreads * queriesPerThread);

    // Verify all rows actually exist in the database
    pool.prepareStatement({"conc_count", "SELECT COUNT(*) FROM pgpp_test_table", {}});
    auto [ok, countRows] = pool.querySync<std::tuple<int>>("conc_count");
    ASSERT_TRUE(ok);
    ASSERT_EQ(countRows.size(), 1u);
    EXPECT_EQ(std::get<0>(countRows[0]), numThreads * queriesPerThread);
}

// ── IT-POOL-008: pool statistics ────────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolStatisticsIdle)
{
    EXPECT_EQ(pool.totalConnections(), 2u);
    EXPECT_EQ(pool.busyConnections(), 0u);
    EXPECT_EQ(pool.freeConnections(), 2u);
    EXPECT_EQ(pool.queuedRequests(), 0u);
    // Invariant: busy + free == total
    EXPECT_EQ(pool.busyConnections() + pool.freeConnections(), pool.totalConnections());
}

TEST_F(PgppIntegrationTest, PoolStatisticsUnderLoad)
{
    pool.prepareStatement({"stat_slow", "SELECT pg_sleep(0.5)", {}});

    // Fire queries to saturate both connections
    auto f1 = pool.execAsync("stat_slow");
    auto f2 = pool.execAsync("stat_slow");
    // This one queues since both executor threads are busy
    auto f3 = pool.execAsync("stat_slow");

    ASSERT_TRUE(waitUntil([&] { return pool.busyConnections() == 2; }));

    // Under load: both connections leased, 1 queued
    EXPECT_EQ(pool.busyConnections(), 2u);
    EXPECT_EQ(pool.freeConnections(), 0u);
    EXPECT_GE(pool.queuedRequests(), 1u);
    EXPECT_EQ(pool.busyConnections() + pool.freeConnections(), pool.totalConnections());

    f1.get(); f2.get(); f3.get();
}

// ── IT-POOL-009: shutdown with pending requests ─────────────────────────────

// Pool of one: the executor thread is busy with the slow statement, so the
// five queued requests can only be drained (nullopt, REQ-PGPP-021). The slow
// statement itself is cancelled by shutdown() and reports false
// (REQ-PGPP-076); nothing waits for pg_sleep to finish.
TEST(PoolShutdown, PendingRequestsGetNullopt)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    pool.prepareStatement({"shutdown_test", "SELECT pg_sleep(5)", {}});

    auto slowFuture = pool.execAsync("shutdown_test");
    ASSERT_TRUE(waitUntil([&] { return pool.busyConnections() == 1; }));

    std::vector<std::future<std::optional<bool>>> pending;
    for (int i = 0; i < 5; ++i)
        pending.push_back(pool.execAsync("shutdown_test"));

    const auto start = std::chrono::steady_clock::now();
    pool.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 2000) << "shutdown() must cancel the running statement, not wait for it";

    for (auto& f : pending) {
        ASSERT_EQ(f.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        EXPECT_EQ(f.get(), std::nullopt) << "a request still queued at shutdown is drained with nullopt";
    }
    ASSERT_EQ(slowFuture.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto slow = slowFuture.get();
    ASSERT_TRUE(slow.has_value()) << "the statement in flight was executed, not drained";
    EXPECT_FALSE(slow.value()) << "a cancelled statement reports failure";
}

// ── IT-POOL-010: prepareStatement on running pool ───────────────────────────

TEST_F(PgppIntegrationTest, PoolPrepareOnRunningPool)
{
    // Pool is already running from SetUp. The statement is usable by the very
    // next request (REQ-PGPP-026): no waiting.
    pool.prepareStatement({"runtime_stmt", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});
    EXPECT_TRUE(pool.execSync("runtime_stmt", std::string("runtime_user")));
}

// ── Missing: pool.query callback API ────────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolQueryCallback)
{
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('cb_query_user', 55)");
    pool.prepareStatement({"cb_query", "SELECT name, score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    std::promise<void> done;
    std::optional<bool> callbackOk;
    std::vector<std::tuple<std::string, int>> callbackRows;

    using Row = std::tuple<std::string, int>;
    pool.query<Row>("cb_query",
        [&](std::optional<bool> ok, std::vector<Row> rows) {
            callbackOk = ok;
            callbackRows = std::move(rows);
            done.set_value();
        },
        std::string("cb_query_user"));

    done.get_future().wait();

    ASSERT_TRUE(callbackOk.has_value());
    EXPECT_TRUE(callbackOk.value());
    ASSERT_EQ(callbackRows.size(), 1u);
    EXPECT_EQ(std::get<0>(callbackRows[0]), "cb_query_user");
    EXPECT_EQ(std::get<1>(callbackRows[0]), 55);
}

// ── Missing: duplicate prepareStatement ─────────────────────────────────────

TEST_F(PgppIntegrationTest, DuplicatePrepareStatementHandled)
{
    // Prepare same name twice — PostgreSQL will reject the second prepare
    // on the same connection, but pool should handle it gracefully
    pool.prepareStatement({"dup_stmt", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});
    EXPECT_TRUE(pool.execSync("dup_stmt", std::string("dup_test_user")));

    // Second registration with the same name: PostgreSQL rejects the duplicate
    // PREPARE on each connection, the first one stays usable.
    pool.prepareStatement({"dup_stmt", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});
    EXPECT_TRUE(pool.execSync("dup_stmt", std::string("dup_test_user_2")));
}

// ── Missing: execPrepared with zero args ────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolExecZeroArgs)
{
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('zero_args', 1)");
    pool.prepareStatement({"zero_sel", "SELECT name FROM pgpp_test_table", {}});

    using Row = std::tuple<std::string>;
    auto [ok, rows] = pool.querySync<Row>("zero_sel");
    EXPECT_TRUE(ok);
    EXPECT_GE(rows.size(), 1u);
}

// ── Single connection pool ─────────────────────────────────────────────────

TEST(PoolSingleConnection, SequentialQueries)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    // Create table (raw TEST — no fixture creates it)
    pool.execRawSync(
        "DO $$ BEGIN "
        "SET LOCAL client_min_messages TO WARNING; "
        "DROP TABLE IF EXISTS pgpp_test_table; "
        "END $$");
    ASSERT_TRUE(pool.execRawSync(
        "CREATE TABLE pgpp_test_table ("
        "  id SERIAL PRIMARY KEY,"
        "  name VARCHAR(255),"
        "  score INTEGER DEFAULT 0,"
        "  rating DOUBLE PRECISION DEFAULT 0.0,"
        "  active BOOLEAN DEFAULT true"
        ")"));

    pool.prepareStatement({"single_ins", "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)", {pg::VARCHAR, pg::INT4}});

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(pool.execSync("single_ins",
            std::string("single_" + std::to_string(i)), std::to_string(i)));
    }

    pool.prepareStatement({"single_count", "SELECT COUNT(*) FROM pgpp_test_table", {}});
    auto [ok, rows] = pool.querySync<std::tuple<int>>("single_count");
    EXPECT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_GE(std::get<0>(rows[0]), 5);

    pool.execRawSync(
        "DO $$ BEGIN "
        "SET LOCAL client_min_messages TO WARNING; "
        "DROP TABLE IF EXISTS pgpp_test_table; "
        "END $$");
    pool.shutdown();
}

// ── Queue saturation: more requests than connections ───────────────────────

TEST_F(PgppIntegrationTest, QueueSaturation)
{
    pool.prepareStatement({"sat_ins", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});

    // Fire 20 async requests on a 2-connection pool
    std::vector<std::future<std::optional<bool>>> futures;
    for (int i = 0; i < 20; ++i)
        futures.push_back(pool.execAsync("sat_ins", std::string("sat_" + std::to_string(i))));

    // All should complete eventually
    for (auto& f : futures) {
        auto result = f.get();
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result.value());
    }
}

// ── Worker recovery after bad query ────────────────────────────────────────

TEST_F(PgppIntegrationTest, WorkerRecoveryAfterBadQuery)
{
    // Execute invalid SQL — should fail
    EXPECT_FALSE(pool.execRawSync("INVALID SQL THAT WILL FAIL"));

    // Pool should still work after the failure
    EXPECT_TRUE(pool.execRawSync("SELECT 1"));
    EXPECT_TRUE(pool.execRawSync("INSERT INTO pgpp_test_table (name) VALUES ('recovery')"));
}

// ── Shutdown during active query ───────────────────────────────────────────

TEST(PoolShutdownActive, ShutdownDuringSlowQuery)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    auto future = pool.execRawAsync("SELECT pg_sleep(2)");
    ASSERT_TRUE(waitUntil([&] { return pool.busyConnections() == 1; }));

    // Shutdown while the statement runs: must not hang
    pool.shutdown();

    auto status = future.wait_for(std::chrono::seconds(5));
    EXPECT_NE(status, std::future_status::timeout) << "Future hung after shutdown";
}

// ── IT-POOL-021: shutdown cancels the statement in flight (REQ-PGPP-076) ───

// A long statement on an executor thread: shutdown() sends a cancel request
// to the leased connection, the statement fails at once and the request
// reports false. Without the cancel, shutdown() waits the full pg_sleep.
TEST(PoolShutdownActive, ShutdownCancelsInFlightStatement)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    auto future = pool.execRawAsync("SELECT pg_sleep(4)");
    ASSERT_TRUE(waitUntil([&] { return pool.busyConnections() == 1; }));

    const auto start = std::chrono::steady_clock::now();
    pool.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 1500) << "shutdown() waited for pg_sleep(4) instead of cancelling it";

    ASSERT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value()) << "the cancelled statement must report failure";
}

// The same for a lease held by one of the caller's own threads: the cancel
// reaches every leased connection, not only the executor's.
TEST(PoolShutdownActive, ShutdownCancelsLeasedStatement)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    std::promise<bool> outcome;
    auto outcomeFuture = outcome.get_future();
    std::thread holder([&] {
        auto lease = pool.acquire();
        outcome.set_value(lease && lease->execRaw("SELECT pg_sleep(4)"));
    });
    ASSERT_TRUE(waitUntil([&] { return pool.busyConnections() == 1; }));

    const auto start = std::chrono::steady_clock::now();
    pool.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    holder.join();

    EXPECT_LT(elapsed.count(), 1500) << "shutdown() waited for the leased statement instead of cancelling it";
    EXPECT_FALSE(outcomeFuture.get()) << "the cancelled statement must report failure";
}

// ── IT-POOL-022: no-result APIs on a statement that returns rows (REQ-PGPP-073)

TEST_F(PgppIntegrationTest, PoolExecOnSelectSucceeds)
{
    pool.prepareStatement({"exec_sel", "SELECT COUNT(*) FROM pgpp_test_table", {}});
    pool.prepareStatement({"exec_ret", "INSERT INTO pgpp_test_table (name) VALUES ($1) RETURNING id", {pg::VARCHAR}});

    EXPECT_TRUE(pool.execSync("exec_sel"));
    EXPECT_TRUE(pool.execSync("exec_ret", std::string("ret_sync")));

    auto future = pool.execAsync("exec_ret", std::string("ret_async"));
    EXPECT_EQ(future.get(), std::optional<bool>(true));

    std::promise<std::optional<bool>> done;
    pool.exec("exec_sel", [&](std::optional<bool> ok) { done.set_value(ok); });
    EXPECT_EQ(done.get_future().get(), std::optional<bool>(true));

    using Row = std::tuple<int64_t>;
    auto [ok, rows] = pool.querySync<Row>("exec_sel");
    ASSERT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 2) << "both INSERT ... RETURNING calls must have happened";
}

// ── IT-POOL-023: bounded queue (REQ-PGPP-078) ───────────────────────────────

// Pool of one whose connection is held by the test: the executor thread takes
// the first request and blocks acquiring; the next `maxQueuedRequests` wait in
// the queue; one more is refused at once with nullopt. Nothing is lost: once
// the connection is returned every accepted request completes.
TEST(PoolQueueLimit, ExcessRequestsAreRefusedAtOnce)
{
    auto info = getTestConnectionInfo();
    info.maxQueuedRequests = 2;
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    std::vector<std::future<std::optional<bool>>> accepted;
    {
        auto held = pool.acquire();
        ASSERT_TRUE(held);

        accepted.push_back(pool.execRawAsync("SELECT 1"));
        ASSERT_TRUE(waitUntil([&] { return pool.queuedRequests() == 0; }))
            << "the executor thread must have taken the first request";
        accepted.push_back(pool.execRawAsync("SELECT 2"));
        accepted.push_back(pool.execRawAsync("SELECT 3"));
        EXPECT_EQ(pool.queuedRequests(), 2u);

        auto refused = pool.execRawAsync("SELECT 4");
        ASSERT_EQ(refused.wait_for(std::chrono::seconds(0)), std::future_status::ready)
            << "a refused request resolves immediately";
        EXPECT_EQ(refused.get(), std::nullopt);
        EXPECT_EQ(pool.queuedRequests(), 2u);

        // The callback API reports the refusal the same way, on the calling thread.
        pool.prepareStatement({"ql_one", "SELECT 1", {}});
        int fired = 0;
        std::optional<bool> seen = true;
        pool.exec("ql_one", [&](std::optional<bool> r) { ++fired; seen = r; });
        EXPECT_EQ(fired, 1);
        EXPECT_EQ(seen, std::nullopt);
    }   // connection returned

    for (auto& f : accepted) {
        EXPECT_EQ(f.get(), std::optional<bool>(true));
    }
    pool.shutdown();
}

// ── IT-POOL-024: default pool size (REQ-PGPP-079) ──────────────────────────

TEST(PoolDefaults, DefaultPoolSizeIsOnePerCoreUpToEight)
{
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(getTestConnectionInfo()));   // poolSize 0 = default
    const size_t cores    = std::thread::hardware_concurrency();
    const size_t expected = cores == 0 ? 8 : std::min<size_t>(cores, 8);
    EXPECT_EQ(pool.totalConnections(), expected);
    pool.shutdown();
}

// ── IT-POOL-025: std::optional through the pool APIs (REQ-PGPP-075) ─────────

TEST_F(PgppIntegrationTest, PoolOptionalParamsAndColumns)
{
    pool.prepareStatement({"popt_ins", "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)", {pg::VARCHAR, pg::INT4}});
    pool.prepareStatement({"popt_sel", "SELECT score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    const std::optional<std::string> null;
    EXPECT_TRUE(pool.execSync("popt_ins", std::string("n"), null));
    EXPECT_EQ(pool.execAsync("popt_ins", std::string("v"), std::optional<std::string>("5")).get(), std::optional<bool>(true));

    using Row = std::tuple<std::optional<int>>;
    auto [ok, rows] = pool.querySync<Row>("popt_sel", std::string("n"));
    ASSERT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), std::nullopt);

    auto [ok2, rows2] = pool.queryAsync<Row>("popt_sel", std::string("v")).get();
    ASSERT_EQ(ok2, std::optional<bool>(true));
    ASSERT_EQ(rows2.size(), 1u);
    EXPECT_EQ(std::get<0>(rows2[0]), std::optional<int>(5));
}

// ── Query returning zero rows via pool ─────────────────────────────────────

TEST_F(PgppIntegrationTest, PoolQueryReturningZeroRows)
{
    pool.prepareStatement({"zero_rows", "SELECT name FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    using Row = std::tuple<std::string>;
    auto [ok, rows] = pool.querySync<Row>("zero_rows", std::string("nonexistent_xyz_999"));
    EXPECT_TRUE(ok);
    EXPECT_TRUE(rows.empty());
}
