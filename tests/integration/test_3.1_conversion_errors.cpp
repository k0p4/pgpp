// IT-CONVERR-001 through IT-CONVERR-008: result conversion failures
//
// A cell that cannot be decoded into the requested C++ type (a bigint above
// INT_MAX read into `int`, a text column read into a number, a numeric
// column, ...) makes decodeBinary return an unexpected ConversionError.
// Every API must turn that into an ordinary failed query (`false`): no
// partial rows, no broken promise, no silently dropped callback, and no
// coroutine left suspended forever.

#include "integration_fixture.h"
#include <pgpp/pgpp_coroutines.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

// Rows 1 and 2 convert fine; the third overflows `int` and must discard
// the two rows already converted.
constexpr const char* OVERFLOW_SQL =
    "SELECT v FROM (VALUES (1::bigint), (2), (3000000000)) t(v) ORDER BY v";

template<typename Row>
void expectConversionFails(PgppConnection& conn, const std::string& name, const std::string& sql)
{
    ASSERT_TRUE(conn.prepare({name, sql, {}})) << sql;
    std::vector<Row> rows;
    EXPECT_FALSE(conn.execPrepared(name, rows)) << sql;
    EXPECT_TRUE(rows.empty()) << sql;
}

bool waitFor(const std::atomic<bool>& flag)
{
    for (int i = 0; i < 500 && !flag.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return flag.load();
}

} // namespace

// ── IT-CONVERR-001: overflow fails, no partial rows, vector appended-only ──

TEST_F(PgppConnectionTest, ConversionOverflowFailsWithoutPartialRows)
{
    ASSERT_TRUE(conn.prepare({"conv_overflow", OVERFLOW_SQL, {}}));

    std::vector<std::tuple<int>> rows { {99} };
    EXPECT_FALSE(conn.execPrepared("conv_overflow", rows));

    // Pre-existing content kept (REQ-PGPP-017), rows 1 and 2 rolled back.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]), 99);

    EXPECT_TRUE(conn.execRaw("SELECT 1")) << "connection must stay usable";
}

// ── IT-CONVERR-002: every decoder reports failure (type mismatch, range, numeric) ──

TEST_F(PgppConnectionTest, ConversionFailureEveryThrowingType)
{
    // out of range for the C++ type
    expectConversionFails<std::tuple<int>>     (conn, "c_int_range", "SELECT 3000000000::bigint");
    expectConversionFails<std::tuple<int16_t>> (conn, "c_i16_range", "SELECT 40000::int4");       // would wrap to -25536
    expectConversionFails<std::tuple<int16_t>> (conn, "c_i16_neg",   "SELECT -40000::int4");
    expectConversionFails<std::tuple<uint32_t>>(conn, "c_u32_neg",   "SELECT -1::int4");           // never wraps to 4294967295
    expectConversionFails<std::tuple<uint32_t>>(conn, "c_u32_range", "SELECT 5000000000::bigint"); // never truncates
    expectConversionFails<std::tuple<float>>   (conn, "c_flt_range", "SELECT 1e39::float8");       // above FLT_MAX
    // column type cannot be read into the C++ type: text into numbers ...
    expectConversionFails<std::tuple<int>>     (conn, "c_int_text",  "SELECT 'abc'::text");
    expectConversionFails<std::tuple<int16_t>> (conn, "c_i16_text",  "SELECT '12'::text");
    expectConversionFails<std::tuple<uint32_t>>(conn, "c_u32_text",  "SELECT 'abc'::text");
    expectConversionFails<std::tuple<double>>  (conn, "c_dbl_text",  "SELECT '1.5'::text");
    // ... numeric (no binary decoder: cast to ::int8 / ::float8 in SQL) ...
    expectConversionFails<std::tuple<int64_t>> (conn, "c_i64_num",   "SELECT 99999999999999999999::numeric");
    expectConversionFails<std::tuple<double>>  (conn, "c_dbl_num",   "SELECT 1.5::numeric");
    // ... and non-textual columns into std::string (cast to ::text in SQL)
    expectConversionFails<std::tuple<std::string>>(conn, "c_str_int", "SELECT 42::int4");
    expectConversionFails<std::tuple<std::string>>(conn, "c_str_ts",  "SELECT now()");
}

// ── IT-CONVERR-003: failure in a later column of a multi-column row ────────

TEST_F(PgppConnectionTest, ConversionFailureInLaterColumn)
{
    expectConversionFails<std::tuple<std::string, int>>(
        conn, "c_second_col", "SELECT 'ok'::text, 'abc'::text");
}

// ── IT-CONVERR-004: querySync returns false instead of throwing ────────────

TEST_F(PgppIntegrationTest, ConversionFailureQuerySync)
{
    pool.prepareStatement({"conv_sync", OVERFLOW_SQL, {}});

    std::pair<bool, std::vector<std::tuple<int>>> result { true, {} };
    ASSERT_NO_THROW(result = pool.querySync<std::tuple<int>>("conv_sync"));
    EXPECT_FALSE(result.first);
    EXPECT_TRUE(result.second.empty());
}

// ── IT-CONVERR-005: queryAsync future resolves with false ──────────────────

TEST_F(PgppIntegrationTest, ConversionFailureQueryAsync)
{
    pool.prepareStatement({"conv_async", OVERFLOW_SQL, {}});

    auto future = pool.queryAsync<std::tuple<int>>("conv_async");
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    std::pair<std::optional<bool>, std::vector<std::tuple<int>>> result;
    ASSERT_NO_THROW(result = future.get()) << "promise must be set, not broken";
    ASSERT_TRUE(result.first.has_value());
    EXPECT_FALSE(result.first.value());
    EXPECT_TRUE(result.second.empty());
}

// ── IT-CONVERR-006: query callback fires with false ────────────────────────

TEST_F(PgppIntegrationTest, ConversionFailureQueryCallback)
{
    pool.prepareStatement({"conv_cb", OVERFLOW_SQL, {}});

    using Row = std::tuple<int>;
    std::promise<std::pair<std::optional<bool>, size_t>> done;
    pool.query<Row>("conv_cb", [&](std::optional<bool> ok, std::vector<Row> rows) {
        done.set_value({ok, rows.size()});
    });

    auto future = done.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "callback must fire";
    auto [ok, rowCount] = future.get();
    ASSERT_TRUE(ok.has_value());
    EXPECT_FALSE(ok.value());
    EXPECT_EQ(rowCount, 0u);
}

// ── IT-CONVERR-007: coQuery resumes the coroutine with false ───────────────

TEST_F(PgppIntegrationTest, ConversionFailureCoQueryResumes)
{
    pool.prepareStatement({"conv_coro", OVERFLOW_SQL, {}});

    std::atomic<bool>   done { false };
    std::optional<bool> queryOk;
    size_t              rowCount = 1;

    auto coro = [&]() -> FireAndForget {
        auto [ok, rows] = co_await coQuery<std::tuple<int>>(pool, "conv_coro");
        queryOk  = ok;
        rowCount = rows.size();
        done.store(true);
    };
    coro();

    ASSERT_TRUE(waitFor(done)) << "coroutine must be resumed, not left suspended";
    ASSERT_TRUE(queryOk.has_value());
    EXPECT_FALSE(queryOk.value());
    EXPECT_EQ(rowCount, 0u);
}

// ── IT-CONVERR-008: worker keeps serving after repeated failures ───────────

TEST(ConversionFailure, SingleWorkerKeepsServing)
{
    PgppPool pool;
    pool.prepareStatement({"conv_many", OVERFLOW_SQL, {}});
    pool.prepareStatement({"conv_good", "SELECT 7", {}});
    ASSERT_TRUE(pool.initialize(getTestConnectionInfo(), 1));

    std::atomic<bool> done { false };
    int failures = 0;
    std::optional<bool> goodOk;
    int goodValue = 0;

    auto coro = [&]() -> FireAndForget {
        for (int i = 0; i < 3; ++i) {
            auto [ok, rows] = co_await coQuery<std::tuple<int>>(pool, "conv_many");
            if (ok.has_value() && !ok.value() && rows.empty()) ++failures;
        }
        auto [ok, rows] = co_await coQuery<std::tuple<int>>(pool, "conv_good");
        goodOk = ok;
        if (!rows.empty()) goodValue = std::get<0>(rows[0]);
        done.store(true);
    };
    coro();

    ASSERT_TRUE(waitFor(done));
    EXPECT_EQ(failures, 3);
    ASSERT_TRUE(goodOk.has_value());
    EXPECT_TRUE(goodOk.value());
    EXPECT_EQ(goodValue, 7);

    pool.shutdown();
}
