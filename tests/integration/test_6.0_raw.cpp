// IT-RAW-001, IT-RAW-002: Raw SQL execution via pool

#include "integration_fixture.h"

// ── IT-RAW-001: execRawSync DDL ─────────────────────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawSyncDDL)
{
    EXPECT_TRUE(pool.execRawSync("CREATE TABLE pgpp_raw_test (id INT)"));
    EXPECT_TRUE(pool.execRawSync("DROP TABLE pgpp_raw_test"));
}

// ── IT-RAW-002: execRawAsync returns future ─────────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawAsyncFuture)
{
    auto future = pool.execRawAsync("CREATE TABLE pgpp_raw_async (id INT)");
    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());

    // Cleanup
    pool.execRawSync("DROP TABLE pgpp_raw_async");
}

// ── Additional: execRawSync with invalid SQL fails ──────────────────────────

TEST_F(PgppIntegrationTest, ExecRawSyncInvalidSQL)
{
    EXPECT_FALSE(pool.execRawSync("THIS IS NOT VALID SQL AT ALL"));
}

// ── Additional: execRawSync SELECT returns true ─────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawSyncSelect)
{
    EXPECT_TRUE(pool.execRawSync("SELECT 1"));
}
