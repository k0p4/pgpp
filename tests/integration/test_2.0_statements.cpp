// IT-STMT-001 through IT-STMT-004: Prepared statement tests

#include "integration_fixture.h"

// ── IT-STMT-001: prepare with valid SQL ─────────────────────────────────────

TEST_F(PgppConnectionTest, PrepareValidSQL)
{
    Statement stmt;
    stmt.statementName = "test_insert";
    stmt.statement = "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)";
    stmt.variables = { pg::VARCHAR, pg::INT4 };

    EXPECT_TRUE(conn.prepare(stmt));
}

// ── IT-STMT-002: prepare with invalid SQL ───────────────────────────────────

TEST_F(PgppConnectionTest, PrepareInvalidSQL)
{
    Statement stmt;
    stmt.statementName = "bad_stmt";
    stmt.statement = "SELECTEROO FROM nonexistent_xyz";
    stmt.variables = {};

    EXPECT_FALSE(conn.prepare(stmt));
}

// ── IT-STMT-003: isPrepared returns true after prepare ──────────────────────

TEST_F(PgppConnectionTest, IsPreparedAfterPrepare)
{
    Statement stmt;
    stmt.statementName = "test_select";
    stmt.statement = "SELECT name FROM pgpp_test_table WHERE id = $1";
    stmt.variables = { pg::INT4 };

    ASSERT_TRUE(conn.prepare(stmt));
    EXPECT_TRUE(conn.isPrepared("test_select"));
}

// ── IT-STMT-004: isPrepared returns false for unknown ───────────────────────

TEST_F(PgppConnectionTest, IsPreparedFalseForUnknown)
{
    EXPECT_FALSE(conn.isPrepared("never_prepared_xyz_999"));
}
