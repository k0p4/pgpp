// IT-TXN-001 through IT-TXN-003: Transaction tests

#include "test_fixture.h"

// ── IT-TXN-001: Transaction commits on success ──────────────────────────────

TEST_F(PgppIntegrationTest, TransactionCommitsOnSuccess)
{
    auto future = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('txn_user', 100)");
    });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());

    // Verify row was committed
    pool.prepareStatement({"txn_check", "SELECT name FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});
    auto [ok, rows] = pool.querySync<std::tuple<std::string>>("txn_check", std::string("txn_user"));
    EXPECT_TRUE(ok);
    EXPECT_EQ(rows.size(), 1u);
}

// ── IT-TXN-002: Transaction rolls back on exception ─────────────────────────

TEST_F(PgppIntegrationTest, TransactionRollsBackOnException)
{
    auto future = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('rollback_user', 200)");
        throw std::runtime_error("intentional rollback");
    });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value());  // should be false (rollback)

    // Verify row was NOT committed
    pool.prepareStatement({"txn_rb_check", "SELECT name FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});
    auto [ok, rows] = pool.querySync<std::tuple<std::string>>("txn_rb_check", std::string("rollback_user"));
    EXPECT_TRUE(ok);
    EXPECT_TRUE(rows.empty());
}

// ── IT-TXN-003: Transaction returns false on failure ────────────────────────

TEST_F(PgppIntegrationTest, TransactionMultiStatement)
{
    auto future = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('multi1', 10)");
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('multi2', 20)");
    });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());

    // Both rows should exist
    pool.prepareStatement({"txn_multi", "SELECT score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    auto [ok1, rows1] = pool.querySync<std::tuple<int>>("txn_multi", std::string("multi1"));
    EXPECT_TRUE(ok1);
    ASSERT_EQ(rows1.size(), 1u);
    EXPECT_EQ(std::get<0>(rows1[0]), 10);

    auto [ok2, rows2] = pool.querySync<std::tuple<int>>("txn_multi", std::string("multi2"));
    EXPECT_TRUE(ok2);
    ASSERT_EQ(rows2.size(), 1u);
    EXPECT_EQ(std::get<0>(rows2[0]), 20);
}
