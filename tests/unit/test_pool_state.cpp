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

TEST(PoolState, DoubleInitializeGuard)
{
    PgppPool pool;
    PgppConnectionInfo info;
    info.dbname = "nonexistent_db_for_test";
    info.host   = "127.0.0.1";
    info.port   = 1; // deliberately wrong port — connection will fail

    // First init will fail (can't connect) but should not crash
    bool first = pool.initialize(info, 1);
    // Result depends on whether PG is reachable — we just care it doesn't crash

    // Second call should return true (already initialized) OR false (first failed, can retry)
    // Either way, no crash or UB
    pool.initialize(info, 1);

    pool.shutdown();
}

// ── UT-POOL-003: Double shutdown is safe ────────────────────────────────────

TEST(PoolState, DoubleShutdownSafe)
{
    PgppPool pool;
    pool.shutdown();  // Not initialized — should be no-op
    pool.shutdown();  // Second call — still safe
    // No crash = pass
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

    pool.initialize(info, 1);
    pool.shutdown();

    // After shutdown, should be able to initialize again
    // (will fail to connect but should not crash or deadlock)
    pool.initialize(info, 1);
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
    // Pool not initialized — enqueue may or may not work depending on shutdown state.
    // But it should not crash.
    (void)enqueued;
}
