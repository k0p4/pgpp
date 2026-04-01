// Compile-time sanity checks: headers compile, key types are well-formed

#include <pgpp/pgpp.h>
#include <pgpp/pgpp_connection.h>
#include <gtest/gtest.h>

#include <type_traits>

// ── Headers compile and key types exist ─────────────────────────────────────

TEST(BasicSanity, TypesAreDefaultConstructible)
{
    EXPECT_TRUE(std::is_default_constructible_v<Statement>);
    EXPECT_TRUE(std::is_default_constructible_v<PgppConnectionInfo>);
    EXPECT_TRUE(std::is_default_constructible_v<PgppPool>);
    EXPECT_TRUE(std::is_default_constructible_v<PgppConnection>);
}

TEST(BasicSanity, PoolIsDestructibleWithoutInit)
{
    // Pool should be safely destructible even if never initialized
    { PgppPool pool; }
    SUCCEED();
}

TEST(BasicSanity, ConnectionIsDestructibleWithoutOpen)
{
    // Connection should be safely destructible even if never opened
    { PgppConnection conn; }
    SUCCEED();
}
