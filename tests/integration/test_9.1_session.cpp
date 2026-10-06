// IT-SESSION-001 through IT-SESSION-004: session state between callers.
//
// Connections have no caller affinity: whatever one request does to its
// session (SET, SET ROLE, LISTEN, a temporary table, an advisory lock) is seen
// by whoever gets that connection next. The pool always rolls back a stray
// transaction (REQ-PGPP-072); with PgppConnectionInfo::resetSessionAfterRequest
// it also resets the session every time a connection is returned
// (REQ-PGPP-080), at the cost of one round trip per request. SET LOCAL inside
// transaction() is the zero-cost alternative.

#include "integration_fixture.h"

#include <atomic>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using NameRow = std::tuple<std::string>;

// Pool of 4, 8 client threads, 250 requests each; one caller does one
// session-level SET. Counts how many later, unrelated requests see it.
int leakedSettings(PgppPool& pool)
{
    pool.prepareStatement({"s_path", "SELECT current_setting('search_path')", {}});
    std::atomic<int> leaked { 0 };
    std::vector<std::thread> clients;
    for (int t = 0; t < 8; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; i < 250; ++i) {
                if (t == 0 && i == 10) {
                    pool.execRawSync("SET search_path TO tenant_42, public");
                }
                auto [ok, rows] = pool.querySync<NameRow>("s_path");
                if (ok && !rows.empty() && std::get<0>(rows[0]).find("tenant_42") != std::string::npos) {
                    ++leaked;
                }
            }
        });
    }
    for (auto& c : clients) c.join();
    return leaked.load();
}

} // namespace

// ── IT-SESSION-001: without the reset, one SET leaks to other callers ───────

// Pinned so the default is documented by a test, not only by prose: the
// setting stays on whichever connection ran the SET and every request that
// lands there afterwards sees it.
TEST(SessionState, SetLeaksBetweenCallersByDefault)
{
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(getTestConnectionInfo(), 4));
    EXPECT_GT(leakedSettings(pool), 0);
    pool.shutdown();
}

// ── IT-SESSION-002: with resetSessionAfterRequest nothing leaks ─────────────

TEST(SessionState, SetDoesNotLeakWithSessionReset)
{
    auto info = getTestConnectionInfo();
    info.resetSessionAfterRequest = true;
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 4));
    EXPECT_EQ(leakedSettings(pool), 0);
    pool.shutdown();
}

// ── IT-SESSION-003: temp tables, LISTEN and advisory locks are undone too ───

TEST(SessionState, TempStateDoesNotLeakWithSessionReset)
{
    auto info = getTestConnectionInfo();
    info.resetSessionAfterRequest = true;
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(info, 1));   // every request lands on the same connection
    pool.prepareStatement({"s_one", "SELECT 1", {}});

    ASSERT_TRUE(pool.execRawSync("CREATE TEMP TABLE pgpp_tmp_leak (v int)"));
    EXPECT_FALSE(pool.execRawSync("SELECT * FROM pgpp_tmp_leak")) << "the temporary table must be gone";

    ASSERT_TRUE(pool.execRawSync("SELECT pg_advisory_lock(424242)"));
    {
        // A second session can take the lock only if the first one released it.
        PgppConnection other;
        ASSERT_TRUE(other.open(PgppPool::buildConnectionString(info)));
        EXPECT_TRUE(other.execRaw("SET lock_timeout = '500ms'; SELECT pg_advisory_lock(424242)"))
            << "the advisory lock must have been released when the connection was returned";
    }

    ASSERT_TRUE(pool.execRawSync("LISTEN pgpp_leak"));
    {
        using Row = std::tuple<int64_t>;
        pool.prepareStatement({"s_listen", "SELECT COUNT(*) FROM pg_listening_channels()", {}});
        auto [ok, rows] = pool.querySync<Row>("s_listen");
        ASSERT_TRUE(ok);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(std::get<0>(rows[0]), 0) << "UNLISTEN * must have run";
    }

    // Prepared statements survive the reset (no DEALLOCATE).
    using One = std::tuple<int>;
    auto [ok, rows] = pool.querySync<One>("s_one");
    EXPECT_TRUE(ok);
    EXPECT_EQ(rows.size(), 1u);
    pool.shutdown();
}

// ── IT-SESSION-004: SET LOCAL inside a transaction never leaks ──────────────

TEST(SessionState, SetLocalInsideTransactionDoesNotLeak)
{
    PgppPool pool;
    ASSERT_TRUE(pool.initialize(getTestConnectionInfo(), 1));
    pool.prepareStatement({"s_path", "SELECT current_setting('search_path')", {}});

    EXPECT_TRUE(pool.transactionSync([](PgppConnection& conn) {
        return conn.execRaw("SET LOCAL search_path TO tenant_7, public");
    }));

    auto [ok, rows] = pool.querySync<NameRow>("s_path");
    ASSERT_TRUE(ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::get<0>(rows[0]).find("tenant_7"), std::string::npos) << "SET LOCAL ends with the transaction";
    pool.shutdown();
}
