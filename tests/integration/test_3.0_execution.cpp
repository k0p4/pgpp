// IT-EXEC-001 through IT-EXEC-009: Direct execution tests

#include "integration_fixture.h"

// ── IT-EXEC-001: execRaw CREATE/DROP ────────────────────────────────────────

TEST_F(PgppConnectionTest, ExecRawCreateDrop)
{
    EXPECT_TRUE(conn.execRaw("CREATE TABLE pgpp_temp_xyz (id INT)"));
    EXPECT_TRUE(conn.execRaw("DROP TABLE pgpp_temp_xyz"));
}

// ── IT-EXEC-002: execPrepared INSERT ────────────────────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedInsert)
{
    Statement stmt;
    stmt.statementName = "ins";
    stmt.statement = "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)";
    stmt.variables = { pg::VARCHAR, pg::INT4 };
    ASSERT_TRUE(conn.prepare(stmt));

    EXPECT_TRUE(conn.execPrepared("ins", std::string("alice"), std::string("100")));
}

// ── IT-EXEC-003: execPrepared SELECT string results ─────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedSelectString)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name) VALUES ('bob')");

    Statement stmt;
    stmt.statementName = "sel_name";
    stmt.statement = "SELECT name FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<std::string>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_name", rows, std::string("bob")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), "bob");
}

// ── IT-EXEC-004: execPrepared SELECT int results ────────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedSelectInt)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('charlie', 42)");

    Statement stmt;
    stmt.statementName = "sel_score";
    stmt.statement = "SELECT score FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<int>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_score", rows, std::string("charlie")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 42);
}

// ── IT-EXEC-005: execPrepared SELECT bool results ───────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedSelectBool)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name, active) VALUES ('dave', true)");
    conn.execRaw("INSERT INTO pgpp_test_table (name, active) VALUES ('eve', false)");

    Statement stmt;
    stmt.statementName = "sel_active";
    stmt.statement = "SELECT active FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<bool>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_active", rows, std::string("dave")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(std::get<0>(rows[0]));

    rows.clear();
    EXPECT_TRUE(conn.execPrepared("sel_active", rows, std::string("eve")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(std::get<0>(rows[0]));
}

// ── IT-EXEC-006: execPrepared SELECT double results ─────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedSelectDouble)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name, rating) VALUES ('frank', 3.14)");

    Statement stmt;
    stmt.statementName = "sel_rating";
    stmt.statement = "SELECT rating FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<double>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_rating", rows, std::string("frank")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(std::get<0>(rows[0]), 3.14, 0.001);
}

// ── IT-EXEC-007: SELECT with NULL handling ──────────────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedNullHandling)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('ghost', NULL)");

    Statement stmt;
    stmt.statementName = "sel_null";
    stmt.statement = "SELECT name, score FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<std::string, int>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_null", rows, std::string("ghost")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), "ghost");
    EXPECT_EQ(std::get<1>(rows[0]), 0);  // NULL -> default int (0)
}

// ── IT-EXEC-008: SELECT multiple rows ───────────────────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedMultipleRows)
{
    conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('a', 1)");
    conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('b', 2)");
    conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('c', 3)");

    Statement stmt;
    stmt.statementName = "sel_all";
    stmt.statement = "SELECT name, score FROM pgpp_test_table ORDER BY score";
    stmt.variables = {};
    ASSERT_TRUE(conn.prepare(stmt));

    std::vector<std::tuple<std::string, int>> rows;
    EXPECT_TRUE(conn.execPrepared("sel_all", rows));
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(std::get<0>(rows[0]), "a");
    EXPECT_EQ(std::get<1>(rows[0]), 1);
    EXPECT_EQ(std::get<0>(rows[2]), "c");
    EXPECT_EQ(std::get<1>(rows[2]), 3);
}

// ── IT-EXEC-009: Wrong param count fails gracefully ─────────────────────────

TEST_F(PgppConnectionTest, ExecPreparedWrongParamCount)
{
    Statement stmt;
    stmt.statementName = "one_param";
    stmt.statement = "SELECT name FROM pgpp_test_table WHERE name = $1";
    stmt.variables = { pg::VARCHAR };
    ASSERT_TRUE(conn.prepare(stmt));

    // Pass no arguments when 1 is expected — should fail, not crash
    EXPECT_FALSE(conn.execPrepared("one_param"));
}
