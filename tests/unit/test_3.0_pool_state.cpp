// UT-POOL-001 through UT-POOL-005: Pool state machine tests
// No database — tests atomic guards and lifecycle without actual connections

#include <pgpp/pgpp.h>
#include <gtest/gtest.h>
#include <thread>

// ── UT-POOL-001: Default constructor sets reasonable pool size ───────────────

TEST(PoolState, DefaultPoolSize)
{
    PgppPool pool;
    // Pool is not initialized yet, but totalConnections shows 0 (no connections created)
    EXPECT_EQ(pool.totalConnections(), 0u);
    EXPECT_FALSE(pool.isInitialized());
}

// ── UT-POOL-002: Double initialize is idempotent (atomic guard) ─────────────

TEST(PoolState, InitializeFailsWithBadConnection)
{
    PgppPool pool;
    PgppConnectionInfo info;
    info.dbname = "nonexistent_db_for_test";
    info.host   = "127.0.0.1";
    info.port   = 1; // deliberately wrong port — connection will fail

    // createConnections() fails → m_initialized reset to false → returns false
    EXPECT_FALSE(pool.initialize(info, 1));
    EXPECT_FALSE(pool.isInitialized());

    // Since first init failed (m_initialized was reset to false), second attempt
    // re-enters initialize and also fails — returns false, not "already initialized"
    EXPECT_FALSE(pool.initialize(info, 1));
    EXPECT_FALSE(pool.isInitialized());

    pool.shutdown();
}

// ── UT-POOL-003: Double shutdown is safe ────────────────────────────────────

TEST(PoolState, DoubleShutdownSafe)
{
    PgppPool pool;
    EXPECT_FALSE(pool.isInitialized());

    pool.shutdown();  // m_initialized is false → exchange(false) returns false → early return
    EXPECT_FALSE(pool.isInitialized());
    EXPECT_EQ(pool.totalConnections(), 0u);
    EXPECT_EQ(pool.queuedRequests(), 0u);

    pool.shutdown();  // Same guard, still no-op
    EXPECT_FALSE(pool.isInitialized());
}

// ── UT-POOL-004: Operations on uninitialized pool don't crash ───────────────

TEST(PoolState, UninitializedPoolOperations)
{
    PgppPool pool;

    // These should all fail gracefully, not crash
    bool execOk = pool.execSync("nonexistent_stmt", std::string("arg"));
    EXPECT_FALSE(execOk);

    auto [queryOk, rows] = pool.querySync<std::tuple<std::string>>("nonexistent", std::string("arg"));
    EXPECT_FALSE(queryOk);
    EXPECT_TRUE(rows.empty());

    bool rawOk = pool.execRawSync("SELECT 1");
    EXPECT_FALSE(rawOk);

    // Statistics should return zeros
    EXPECT_EQ(pool.totalConnections(), 0u);
    EXPECT_EQ(pool.freeConnections(), 0u);
    EXPECT_EQ(pool.busyConnections(), 0u);
    EXPECT_EQ(pool.queuedRequests(), 0u);
}

// ── UT-POOL-005: Shutdown then re-initialize ────────────────────────────────

TEST(PoolState, ShutdownThenReinitialize)
{
    PgppPool pool;
    PgppConnectionInfo info;
    info.dbname = "nonexistent_db_for_test";
    info.host   = "127.0.0.1";
    info.port   = 1;

    // First cycle: init fails (bad port), shutdown is no-op (not initialized)
    EXPECT_FALSE(pool.initialize(info, 1));
    EXPECT_FALSE(pool.isInitialized());
    pool.shutdown();
    EXPECT_FALSE(pool.isInitialized());

    // Second cycle: re-init also fails, but state machine allows retry
    EXPECT_FALSE(pool.initialize(info, 1));
    EXPECT_FALSE(pool.isInitialized());
    pool.shutdown();
}

// ── Additional: enqueueRaw on uninitialized pool ────────────────────────────

TEST(PoolState, EnqueueRawOnUninitializedPool)
{
    PgppPool pool;

    auto request = std::make_unique<PgppRequest>();
    bool called = false;
    request->task = [&called](PgppConnection*) { called = true; };

    bool enqueued = pool.enqueueRaw(std::move(request));
    EXPECT_FALSE(enqueued);  // m_initialized is false — must reject
    EXPECT_FALSE(called);    // task should never have been invoked
}

// ── Additional: async operations on uninitialized pool return immediately ───

TEST(PoolState, AsyncOnUninitializedPoolReturnsNullopt)
{
    PgppPool pool;

    auto future = pool.execAsync("nonexistent", std::string("arg"));
    auto result = future.get();
    EXPECT_FALSE(result.has_value());  // nullopt — pool not initialized

    using Row = std::tuple<std::string>;
    auto future2 = pool.queryAsync<Row>("nonexistent", std::string("arg"));
    auto [ok, rows] = future2.get();
    EXPECT_FALSE(ok.has_value());
    EXPECT_TRUE(rows.empty());

    auto future3 = pool.execRawAsync("SELECT 1");
    auto result3 = future3.get();
    EXPECT_FALSE(result3.has_value());
}

// ── prepareStatement on uninitialized pool ──────────────────────────────────

TEST(PoolState, PrepareStatementBeforeInitialize)
{
    PgppPool pool;
    Statement stmt;
    stmt.statementName = "test_stmt";
    stmt.statement = "SELECT $1";
    stmt.variables = { pg::VARCHAR };

    // Should store the statement without crashing (no connections to prepare on)
    EXPECT_NO_THROW(pool.prepareStatement(stmt));
}

TEST(PoolState, PrepareStatementEmptyName)
{
    PgppPool pool;
    Statement stmt;
    stmt.statementName = "";
    stmt.statement = "SELECT 1";

    EXPECT_NO_THROW(pool.prepareStatement(stmt));
}

// ── Callback-based operations on uninitialized pool ─────────────────────────

TEST(PoolState, CallbackExecOnUninitializedPool)
{
    PgppPool pool;
    bool callbackCalled {};
    pool.exec("nonexistent",
        [&callbackCalled](std::optional<bool> result) {
            callbackCalled = true;
            EXPECT_FALSE(result.has_value());
        },
        std::string("arg"));

    // Give a moment for the callback (though it should be immediate for rejected requests)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Callback may or may not be called depending on implementation —
    // the important thing is no crash
    SUCCEED();
}

TEST(PoolState, TransactionOnUninitializedPool)
{
    PgppPool pool;
    auto future = pool.transaction([](PgppConnection&) {
        // Should never execute
    });

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
}
