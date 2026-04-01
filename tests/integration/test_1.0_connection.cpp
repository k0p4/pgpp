// IT-CONN-001 through IT-CONN-006: Connection lifecycle tests

#include "integration_fixture.h"
#include <chrono>

// ── IT-CONN-001: open with valid string ─────────────────────────────────────

TEST(ConnectionLifecycle, OpenValidConnection)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    EXPECT_TRUE(conn.open(cs));
    EXPECT_TRUE(conn.isOpen());
    conn.close();
}

// ── IT-CONN-002: open with invalid string ───────────────────────────────────

TEST(ConnectionLifecycle, OpenInvalidConnection)
{
    PgppConnection conn;
    EXPECT_FALSE(conn.open("dbname='nonexistent_db_xyz' host='127.0.0.1' port=1"));
    EXPECT_FALSE(conn.isOpen());
}

// ── IT-CONN-003: isOpen after open ──────────────────────────────────────────

TEST(ConnectionLifecycle, IsOpenAfterOpen)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));
    EXPECT_TRUE(conn.isOpen());
    conn.close();
}

// ── IT-CONN-004: close sets isOpen false, double close safe ─────────────────

TEST(ConnectionLifecycle, CloseAndDoubleClose)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));
    conn.close();
    EXPECT_FALSE(conn.isOpen());

    // Double close should be safe
    conn.close();
    EXPECT_FALSE(conn.isOpen());
}

// ── IT-CONN-005: reset restores connection ──────────────────────────────────

TEST(ConnectionLifecycle, ResetRestoresConnection)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));
    EXPECT_TRUE(conn.isOpen());

    // Reset should keep connection alive
    conn.reset();
    EXPECT_TRUE(conn.isOpen());

    // Should still be able to query after reset
    EXPECT_TRUE(conn.execRaw("SELECT 1"));
    conn.close();
}

// ── IT-CONN-006: lastError returns message after failure ────────────────────

TEST(ConnectionLifecycle, LastErrorAfterFailure)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));

    // Execute invalid SQL
    bool ok = conn.execRaw("INVALID SQL SYNTAX HERE !!!");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(conn.lastError().empty());

    conn.close();
}

// ── Reconnect after close ──────────────────────────────────────────────────

TEST(ConnectionLifecycle, ReconnectAfterClose)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));
    conn.close();
    EXPECT_FALSE(conn.isOpen());

    // Re-open on the same object
    EXPECT_TRUE(conn.open(cs));
    EXPECT_TRUE(conn.isOpen());
    EXPECT_TRUE(conn.execRaw("SELECT 1"));
    conn.close();
}

// ── Open twice without close ───────────────────────────────────────────────

TEST(ConnectionLifecycle, OpenTwiceWithoutClose)
{
    auto info = getTestConnectionInfo();
    std::string cs = "dbname='" + info.dbname + "' host='" + info.host + "' "
                   + "port=" + std::to_string(info.port) + " "
                   + "user='" + info.user + "' password='" + info.password + "'";

    PgppConnection conn;
    ASSERT_TRUE(conn.open(cs));

    // Second open without close — document behavior
    bool secondOpen = conn.open(cs);
    // Either succeeds (replaces) or fails (already open) — neither should crash
    EXPECT_TRUE(conn.isOpen());
    (void)secondOpen;
    conn.close();
}

// ── Invalid host fast fail ─────────────────────────────────────────────────

TEST(ConnectionLifecycle, InvalidHostFastFail)
{
    PgppConnection conn;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(conn.open("dbname='test' host='127.0.0.1' port=1"));
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should fail within a reasonable time, not hang
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10);
    EXPECT_FALSE(conn.isOpen());
}
