// UT-CONN-001 through UT-CONN-003: Connection string builder tests
// Uses PGPP_TESTING friend access to call private buildConnectionString

#include <pgpp/pgpp.h>
#include <gtest/gtest.h>

class PgppPoolTest : public ::testing::Test {
protected:
    PgppPool pool;

    std::string build(const PgppConnectionInfo& info) {
        return pool.buildConnectionString(info);
    }
};

// ── UT-CONN-001: All fields populated ───────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringAllFields)
{
    PgppConnectionInfo info;
    info.dbname   = "testdb";
    info.host     = "localhost";
    info.user     = "testuser";
    info.password = "secret";
    info.port     = 5432;
    info.sslmode  = "prefer";

    std::string result = build(info);

    EXPECT_NE(result.find("host="), std::string::npos);
    EXPECT_NE(result.find("dbname="), std::string::npos);
    EXPECT_NE(result.find("user="), std::string::npos);
    EXPECT_NE(result.find("password="), std::string::npos);
    EXPECT_NE(result.find("port=5432"), std::string::npos);
    EXPECT_NE(result.find("sslmode="), std::string::npos);
    EXPECT_FALSE(result.empty());
}

// ── UT-CONN-002: Empty dbname returns empty string ──────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringEmptyDbname)
{
    PgppConnectionInfo info;
    info.host = "localhost";
    info.user = "testuser";
    // dbname left empty

    std::string result = build(info);
    EXPECT_TRUE(result.empty());
}

// ── UT-CONN-003: Special chars in password are escaped ──────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringEscapesSpecialChars)
{
    PgppConnectionInfo info;
    info.dbname   = "testdb";
    info.host     = "localhost";
    info.user     = "admin";
    info.password = "pass'word\\with\"special";

    std::string result = build(info);

    // Single quotes and backslashes should be escaped
    // The password field should contain escaped versions
    EXPECT_NE(result.find("password="), std::string::npos);
    // Original unescaped chars should NOT appear bare
    // (they should be preceded by backslash)
    EXPECT_NE(result.find("\\'"), std::string::npos);
    EXPECT_NE(result.find("\\\\"), std::string::npos);
}

// ── Additional edge cases ───────────────────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringNoUser)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    // no user/password

    std::string result = build(info);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.find("user="), std::string::npos);
    EXPECT_EQ(result.find("password="), std::string::npos);
}

TEST_F(PgppPoolTest, BuildConnectionStringUserNoPassword)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    info.user   = "admin";
    // password left empty

    std::string result = build(info);
    EXPECT_NE(result.find("user="), std::string::npos);
    EXPECT_EQ(result.find("password="), std::string::npos);
}

TEST_F(PgppPoolTest, BuildConnectionStringPortZeroOmitted)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    info.port   = 0;

    std::string result = build(info);
    EXPECT_EQ(result.find("port="), std::string::npos);
}
