// IT-TXN-001 through IT-TXN-009: Transaction tests

#include "integration_fixture.h"
#include <thread>
#include <future>
#include <chrono>

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

// ── IT-TXN-004: Constraint violation causes rollback ───────────────────────

TEST_F(PgppIntegrationTest, TransactionConstraintViolationRollback)
{
    // Insert a row first
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('unique_user', 1)");

    // Add unique constraint
    pool.execRawSync("CREATE UNIQUE INDEX IF NOT EXISTS pgpp_test_name_uniq ON pgpp_test_table(name)");

    auto future = pool.transaction([](PgppConnection& conn) {
        // This should fail — duplicate name violates unique constraint
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('unique_user', 2)");
    });

    auto result = future.get();
    // The INSERT fails and execRaw returns false without throwing, which leaves
    // the transaction aborted. COMMIT on an aborted transaction answers
    // "ROLLBACK" with PGRES_COMMAND_OK, so the pool must detect the aborted
    // state itself and report false (REQ-PGPP-065).
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value());

    // Cleanup
    pool.execRawSync("DROP INDEX IF EXISTS pgpp_test_name_uniq");

    // Verify no duplicate row with score=2
    pool.prepareStatement({"txn_uniq", "SELECT score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});
    auto [ok, rows] = pool.querySync<std::tuple<int>>("txn_uniq", std::string("unique_user"));
    EXPECT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 1); // original value, not the duplicate
}

// ── IT-TXN-005: Empty transaction commits successfully ─────────────────────

TEST_F(PgppIntegrationTest, TransactionEmptyCommits)
{
    auto future = pool.transaction([](PgppConnection&) {
        // No statements — just BEGIN; COMMIT
    });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());
}

// ── IT-TXN-006: Long transaction with many statements ──────────────────────

TEST_F(PgppIntegrationTest, TransactionManyStatements)
{
    auto future = pool.transaction([](PgppConnection& conn) {
        for (int i = 0; i < 50; ++i) {
            std::string sql = "INSERT INTO pgpp_test_table (name, score) VALUES ('txn_batch_"
                            + std::to_string(i) + "', " + std::to_string(i) + ")";
            conn.execRaw(sql.c_str());
        }
    });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());

    // Verify all 50 rows committed
    pool.prepareStatement({"txn_batch_count",
        "SELECT COUNT(*) FROM pgpp_test_table WHERE name LIKE 'txn_batch_%'", {}});
    auto [ok, rows] = pool.querySync<std::tuple<int>>("txn_batch_count");
    EXPECT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 50);
}

// ── IT-TXN-007: Deadlock handling ──────────────────────────────────────────

TEST_F(PgppIntegrationTest, TransactionDeadlockHandling)
{
    // Set up: two rows to cross-lock
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('lock_a', 1)");
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('lock_b', 2)");

    // Transaction 1: lock row A, then try row B
    auto f1 = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("UPDATE pgpp_test_table SET score = 10 WHERE name = 'lock_a'");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        conn.execRaw("UPDATE pgpp_test_table SET score = 20 WHERE name = 'lock_b'");
    });

    // Transaction 2: lock row B, then try row A (reverse order → deadlock)
    auto f2 = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("UPDATE pgpp_test_table SET score = 30 WHERE name = 'lock_b'");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        conn.execRaw("UPDATE pgpp_test_table SET score = 40 WHERE name = 'lock_a'");
    });

    // Both futures must resolve — PostgreSQL detects deadlock and aborts one
    auto status1 = f1.wait_for(std::chrono::seconds(10));
    auto status2 = f2.wait_for(std::chrono::seconds(10));

    EXPECT_NE(status1, std::future_status::timeout) << "Transaction 1 hung on deadlock";
    EXPECT_NE(status2, std::future_status::timeout) << "Transaction 2 hung on deadlock";

    // PostgreSQL aborts exactly one side (its second UPDATE fails with a
    // deadlock error); that side must report false, the other commits.
    auto r1 = f1.get();
    auto r2 = f2.get();
    ASSERT_TRUE(r1.has_value() && r2.has_value());
    EXPECT_NE(r1.value(), r2.value()) << "exactly one side of the deadlock may commit";

    // The rows must reflect exactly one transaction: (10, 20) if the first won,
    // (40, 30) if the second. Any mix means a half-applied transaction.
    pool.prepareStatement({"txn_lock_score", "SELECT score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});
    auto [okA, rowsA] = pool.querySync<std::tuple<int>>("txn_lock_score", std::string("lock_a"));
    auto [okB, rowsB] = pool.querySync<std::tuple<int>>("txn_lock_score", std::string("lock_b"));
    ASSERT_TRUE(okA && okB);
    ASSERT_EQ(rowsA.size(), 1u);
    ASSERT_EQ(rowsB.size(), 1u);
    const int scoreA = std::get<0>(rowsA[0]);
    const int scoreB = std::get<0>(rowsB[0]);
    if (r1.value()) {
        EXPECT_EQ(scoreA, 10);
        EXPECT_EQ(scoreB, 20);
    } else {
        EXPECT_EQ(scoreA, 40);
        EXPECT_EQ(scoreB, 30);
    }
}

// ── IT-TXN-008: A failed statement rolls the whole transaction back ────────

TEST_F(PgppIntegrationTest, TransactionFailedStatementRollsBack)
{
    // The wallet example: the first UPDATE violates a CHECK constraint, the
    // second would succeed on its own. Neither may be applied, and the result
    // must say so even though no statement result is checked inside `work`.
    ASSERT_TRUE(pool.execRawSync("ALTER TABLE pgpp_test_table ADD CONSTRAINT pgpp_test_score_nonneg CHECK (score >= 0)"));
    ASSERT_TRUE(pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('wallet_1', 50), ('wallet_2', 0)"));

    auto result = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("UPDATE pgpp_test_table SET score = score - 100 WHERE name = 'wallet_1'");
        conn.execRaw("UPDATE pgpp_test_table SET score = score + 100 WHERE name = 'wallet_2'");
    }).get();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value());

    pool.prepareStatement({"txn_wallet", "SELECT score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});
    auto [ok1, rows1] = pool.querySync<std::tuple<int>>("txn_wallet", std::string("wallet_1"));
    auto [ok2, rows2] = pool.querySync<std::tuple<int>>("txn_wallet", std::string("wallet_2"));
    ASSERT_TRUE(ok1 && ok2);
    ASSERT_EQ(rows1.size(), 1u);
    ASSERT_EQ(rows2.size(), 1u);
    EXPECT_EQ(std::get<0>(rows1[0]), 50);
    EXPECT_EQ(std::get<0>(rows2[0]), 0);
}

// ── IT-TXN-009: `work` returning bool ───────────────────────────────────────

TEST_F(PgppIntegrationTest, TransactionWorkReturnsBool)
{
    pool.prepareStatement({"txn_bool_check", "SELECT name FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    // false: the explicit, non-throwing way to abort (REQ-PGPP-066).
    auto aborted = pool.transaction([](PgppConnection& conn) {
        conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('bool_abort', 1)");
        return false;
    }).get();
    ASSERT_TRUE(aborted.has_value());
    EXPECT_FALSE(aborted.value());

    auto [okA, rowsA] = pool.querySync<std::tuple<std::string>>("txn_bool_check", std::string("bool_abort"));
    EXPECT_TRUE(okA);
    EXPECT_TRUE(rowsA.empty());

    // true: committed like a void work.
    auto committed = pool.transaction([](PgppConnection& conn) {
        return conn.execRaw("INSERT INTO pgpp_test_table (name, score) VALUES ('bool_commit', 1)");
    }).get();
    ASSERT_TRUE(committed.has_value());
    EXPECT_TRUE(committed.value());

    auto [okC, rowsC] = pool.querySync<std::tuple<std::string>>("txn_bool_check", std::string("bool_commit"));
    EXPECT_TRUE(okC);
    EXPECT_EQ(rowsC.size(), 1u);
}
