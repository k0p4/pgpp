// IT-CORO-001 through IT-CORO-003: Coroutine tests

#include "test_fixture.h"
#include <pgpp/pgpp_coroutines.h>
#include <atomic>
#include <chrono>
#include <thread>

// ── IT-CORO-001: coExec executes INSERT ─────────────────────────────────────

TEST_F(PgppIntegrationTest, CoExecInsert)
{
    pool.prepareStatement({"coro_ins", "INSERT INTO pgpp_test_table (name, score) VALUES ($1, $2)", {pg::VARCHAR, pg::INT4}});

    std::atomic<bool> done { false };
    std::optional<bool> result;

    auto coro = [&]() -> FireAndForget {
        result = co_await coExec(pool, "coro_ins", std::string("coro_user"), std::string("42"));
        done.store(true);
    };
    coro();

    // Wait for coroutine to complete
    for (int i = 0; i < 50 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(done.load());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());
}

// ── IT-CORO-002: coQuery returns typed rows ─────────────────────────────────

TEST_F(PgppIntegrationTest, CoQueryReturnsRows)
{
    pool.execRawSync("INSERT INTO pgpp_test_table (name, score) VALUES ('coro_bob', 77)");
    pool.prepareStatement({"coro_sel", "SELECT name, score FROM pgpp_test_table WHERE name = $1", {pg::VARCHAR}});

    std::atomic<bool> done { false };
    std::optional<bool> queryOk;
    std::vector<std::tuple<std::string, int>> queryRows;

    auto coro = [&]() -> FireAndForget {
        using Row = std::tuple<std::string, int>;
        auto [ok, rows] = co_await coQuery<Row>(pool, "coro_sel", std::string("coro_bob"));
        queryOk = ok;
        queryRows = std::move(rows);
        done.store(true);
    };
    coro();

    for (int i = 0; i < 50 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(done.load());
    ASSERT_TRUE(queryOk.has_value());
    EXPECT_TRUE(queryOk.value());
    ASSERT_EQ(queryRows.size(), 1u);
    EXPECT_EQ(std::get<0>(queryRows[0]), "coro_bob");
    EXPECT_EQ(std::get<1>(queryRows[0]), 77);
}

// ── IT-CORO-003: FireAndForget self-destructs ───────────────────────────────

TEST_F(PgppIntegrationTest, FireAndForgetSelfDestructs)
{
    pool.prepareStatement({"coro_faf", "INSERT INTO pgpp_test_table (name) VALUES ($1)", {pg::VARCHAR}});

    std::atomic<int> completionCount { 0 };

    // Launch multiple fire-and-forget coroutines
    for (int i = 0; i < 10; ++i) {
        auto name = std::string("faf_") + std::to_string(i);
        [&, name]() -> FireAndForget {
            co_await coExec(pool, "coro_faf", name);
            completionCount.fetch_add(1);
        }();
    }

    // Wait for all to complete
    for (int i = 0; i < 100 && completionCount.load() < 10; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(completionCount.load(), 10);
}
