// IT-POOL-001 through IT-POOL-010: Pool operations tests

#include "integration_fixture.h"
#include <thread>
#include <atomic>
#include <vector>
#include <future>
#include <chrono>

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
    // These will queue since both workers are busy
    auto f3 = pool.execAsync("stat_slow");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Under load: both workers busy, 1 queued
    EXPECT_EQ(pool.busyConnections(), 2u);
    EXPECT_EQ(pool.freeConnections(), 0u);
    EXPECT_GE(pool.queuedRequests(), 1u);
    EXPECT_EQ(pool.busyConnections() + pool.freeConnections(), pool.totalConnections());

    f1.get(); f2.get(); f3.get();
}

// ── IT-POOL-009: shutdown with pending requests ─────────────────────────────

TEST(PoolShutdown, PendingRequestsGetNullopt)
{
    auto info = getTestConnectionInfo();
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));

    pool.prepareStatement({"shutdown_test", "SELECT pg_sleep(5)", {}});

    // Fire a slow query
    auto slowFuture = pool.execAsync("shutdown_test");

    // Immediately fire more queries that will queue
    std::vector<std::future<std::optional<bool>>> pending;
    for (int i = 0; i < 5; ++i)
        pending.push_back(pool.execAsync("shutdown_test"));

    // Shutdown while queries are pending
    pool.shutdown();

    // Pending futures must resolve (not hang) and return nullopt or false
    for (auto& f : pending) {
        auto result = f.get();
        EXPECT_TRUE(!result.has_value() || !result.value())
            << "Pending request after shutdown should be nullopt or false";
    }
}

// ── IT-POOL-010: prepareStatement on running pool ───────────────────────────

TEST_F(PgppIntegrationTest, PoolPrepareOnRunningPool)
{
    // Pool is already running from SetUp. Add a new statement.
    pool.prepareStatement({"runtime_stmt", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});

    // Give workers time to prepare
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second prepare with same name — should not crash
    pool.prepareStatement({"dup_stmt", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Statement should still work
    EXPECT_TRUE(pool.execSync("dup_stmt", std::string("dup_test_user")));
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

    // Fire a slow query
    auto future = pool.execRawAsync("SELECT pg_sleep(2)");

    // Shutdown immediately — should not hang
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pool.shutdown();

    // Future must resolve within reasonable time
    auto status = future.wait_for(std::chrono::seconds(5));
    EXPECT_NE(status, std::future_status::timeout) << "Future hung after shutdown";
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
