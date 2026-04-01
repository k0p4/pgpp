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

// ── Multi-statement raw SQL ────────────────────────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawMultiStatement)
{
    // Multiple statements in one call
    bool ok = pool.execRawSync(
        "INSERT INTO pgpp_test_table (name, score) VALUES ('raw_multi1', 1);"
        "INSERT INTO pgpp_test_table (name, score) VALUES ('raw_multi2', 2);");

    // libpq executes multi-statement — verify at least one row exists
    if (ok) {
        pool.prepareStatement({"raw_multi_check",
            "SELECT COUNT(*) FROM pgpp_test_table WHERE name LIKE 'raw_multi%'", {}});
        auto [qok, rows] = pool.querySync<std::tuple<int>>("raw_multi_check");
        EXPECT_TRUE(qok);
        EXPECT_GE(std::get<0>(rows[0]), 1);
    }
}

// ── DDL sequence ───────────────────────────────────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawDDLSequence)
{
    EXPECT_TRUE(pool.execRawSync("CREATE TABLE pgpp_ddl_test (id INT, val TEXT)"));
    EXPECT_TRUE(pool.execRawSync("ALTER TABLE pgpp_ddl_test ADD COLUMN extra INT DEFAULT 0"));
    EXPECT_TRUE(pool.execRawSync("INSERT INTO pgpp_ddl_test (id, val) VALUES (1, 'test')"));
    EXPECT_TRUE(pool.execRawSync("DROP TABLE pgpp_ddl_test"));
}

// ── Partial failure: valid then invalid SQL ────────────────────────────────

TEST_F(PgppIntegrationTest, ExecRawPartialFailure)
{
    // Insert a valid row first
    EXPECT_TRUE(pool.execRawSync("INSERT INTO pgpp_test_table (name) VALUES ('before_fail')"));

    // Now execute invalid SQL — should fail
    EXPECT_FALSE(pool.execRawSync("ABSOLUTELY INVALID SQL GARBAGE"));

    // Pool should still work after the failure
    EXPECT_TRUE(pool.execRawSync("INSERT INTO pgpp_test_table (name) VALUES ('after_fail')"));
}
