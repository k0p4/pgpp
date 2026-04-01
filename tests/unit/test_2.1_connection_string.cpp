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
    info.password = "pass'word\\end";

    std::string result = build(info);

    // Password field must contain escaped single-quote and backslash inside single quotes
    // Expected: password='pass\'word\\end'
    EXPECT_NE(result.find("password='pass\\'word\\\\end'"), std::string::npos)
        << "Actual connection string: " << result;
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

// ── Options field ───────────────────────────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringOptionsIncluded)
{
    PgppConnectionInfo info;
    info.dbname  = "testdb";
    info.host    = "localhost";
    info.options = "-c search_path=myschema";

    std::string result = build(info);
    EXPECT_NE(result.find("options="), std::string::npos);
}

TEST_F(PgppPoolTest, BuildConnectionStringSslmodeIncluded)
{
    PgppConnectionInfo info;
    info.dbname  = "testdb";
    info.host    = "localhost";
    info.sslmode = "require";

    std::string result = build(info);
    EXPECT_NE(result.find("sslmode="), std::string::npos);
    EXPECT_NE(result.find("require"), std::string::npos);
}

// ── Special characters ──────────────────────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringMixedSpecialChars)
{
    PgppConnectionInfo info;
    info.dbname   = "testdb";
    info.host     = "localhost";
    info.user     = "admin";
    info.password = "p'a\\ss'w\\ord";

    std::string result = build(info);
    // All single quotes and backslashes must be escaped
    EXPECT_NE(result.find("password="), std::string::npos);
    // Original ' and \ should not appear unescaped
    EXPECT_EQ(result.find("password='p'a"), std::string::npos)
        << "Unescaped quote found in: " << result;
}

TEST_F(PgppPoolTest, BuildConnectionStringUsernameWithSpaces)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    info.user   = "my user";

    std::string result = build(info);
    EXPECT_NE(result.find("user='my user'"), std::string::npos)
        << "Username with spaces should be quoted: " << result;
}

// ── Port boundaries ─────────────────────────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringPortMax)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    info.port   = 65535;

    std::string result = build(info);
    EXPECT_NE(result.find("port=65535"), std::string::npos);
}

TEST_F(PgppPoolTest, BuildConnectionStringPortMin)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    info.port   = 1;

    std::string result = build(info);
    EXPECT_NE(result.find("port=1 "), std::string::npos);
}

// ── Minimal fields ──────────────────────────────────────────────────────────

TEST_F(PgppPoolTest, BuildConnectionStringMinimalFields)
{
    PgppConnectionInfo info;
    info.dbname = "testdb";
    info.host   = "localhost";
    // all optional fields left empty, port at default

    std::string result = build(info);
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("dbname="), std::string::npos);
    EXPECT_NE(result.find("host="), std::string::npos);
    EXPECT_EQ(result.find("user="), std::string::npos);
    EXPECT_EQ(result.find("password="), std::string::npos);
    EXPECT_EQ(result.find("sslmode="), std::string::npos);
    EXPECT_EQ(result.find("options="), std::string::npos);
}

TEST_F(PgppPoolTest, BuildConnectionStringUnicodeDbname)
{
    PgppConnectionInfo info;
    info.dbname = "\xd1\x82\xd0\xb5\xd1\x81\xd1\x82"; // тест (Cyrillic)
    info.host   = "localhost";

    std::string result = build(info);
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("dbname="), std::string::npos);
}
