// UT-STMT-001, UT-OID-001: Struct and OID constant tests

#include <pgpp/pgpp.h>
#include <gtest/gtest.h>

// ── UT-STMT-001: Statement struct holds fields correctly ────────────────────

TEST(Structs, StatementHoldsFields)
{
    Statement stmt;
    stmt.statementName = "find_user";
    stmt.statement = "SELECT login, email FROM account WHERE login = $1";
    stmt.variables = { pg::VARCHAR };

    EXPECT_EQ(stmt.statementName, "find_user");
    EXPECT_EQ(stmt.statement, "SELECT login, email FROM account WHERE login = $1");
    ASSERT_EQ(stmt.variables.size(), 1u);
    EXPECT_EQ(stmt.variables[0], pg::VARCHAR);
}

TEST(Structs, StatementMultipleParams)
{
    Statement stmt;
    stmt.statementName = "insert_account";
    stmt.statement = "INSERT INTO account (login, password, email) VALUES ($1, $2, $3)";
    stmt.variables = { pg::VARCHAR, pg::VARCHAR, pg::VARCHAR };

    EXPECT_EQ(stmt.variables.size(), 3u);
}

TEST(Structs, PgppConnectionInfoDefaults)
{
    PgppConnectionInfo info;
    EXPECT_TRUE(info.dbname.empty());
    EXPECT_TRUE(info.host.empty());
    EXPECT_TRUE(info.user.empty());
    EXPECT_TRUE(info.password.empty());
    EXPECT_EQ(info.port, 5432);
}

// ── UT-OID-001: pg:: namespace OID constants match PostgreSQL catalog ───────

TEST(OIDConstants, MatchPostgreSQLCatalog)
{
    // Values from PostgreSQL pg_type system catalog
    EXPECT_EQ(pg::BYTEA,       17u);
    EXPECT_EQ(pg::CHAR,        18u);
    EXPECT_EQ(pg::INT8,        20u);
    EXPECT_EQ(pg::INT2,        21u);
    EXPECT_EQ(pg::INT4,        23u);
    EXPECT_EQ(pg::TEXT,        25u);
    EXPECT_EQ(pg::FLOAT4,     700u);
    EXPECT_EQ(pg::FLOAT8,     701u);
    EXPECT_EQ(pg::VARCHAR,    1043u);
    EXPECT_EQ(pg::DATE,       1082u);
    EXPECT_EQ(pg::TIME,       1083u);
    EXPECT_EQ(pg::TIMESTAMP,  1114u);
    EXPECT_EQ(pg::TIMESTAMPTZ, 1184u);
    EXPECT_EQ(pg::TIMETZ,     1266u);
}
