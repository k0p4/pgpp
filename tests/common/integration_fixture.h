#pragma once

#include "test_config.h"
#include <pgpp/pgpp.h>
#include <gtest/gtest.h>
#include <string>

// Shared fixture for integration tests.
// Reads connection info from environment variables or uses defaults.
// Creates a test table in SetUp, drops it in TearDown.

using pgpp_test::getTestConnectionInfo;

class PgppIntegrationTest : public ::testing::Test {
protected:
    PgppPool pool;
    PgppConnectionInfo connInfo;

    void SetUp() override
    {
        connInfo = getTestConnectionInfo();
        ASSERT_TRUE(pool.initialize(connInfo, 2))
            << "Cannot connect to PostgreSQL. Set PGPP_TEST_* env vars.";

        pool.execRawSync("DROP TABLE IF EXISTS pgpp_test_table");
        ASSERT_TRUE(pool.execRawSync(
            "CREATE TABLE pgpp_test_table ("
            "  id SERIAL PRIMARY KEY,"
            "  name VARCHAR(255),"
            "  score INTEGER DEFAULT 0,"
            "  rating DOUBLE PRECISION DEFAULT 0.0,"
            "  active BOOLEAN DEFAULT true"
            ")"));
    }

    void TearDown() override
    {
        pool.execRawSync("DROP TABLE IF EXISTS pgpp_test_table");
        pool.shutdown();
    }
};

// Fixture for single-connection tests (no pool)
class PgppConnectionTest : public ::testing::Test {
protected:
    PgppConnection conn;
    PgppConnectionInfo connInfo;
    std::string connString;

    void SetUp() override
    {
        connInfo = getTestConnectionInfo();
        // Build connection string manually
        connString = "dbname='" + connInfo.dbname + "' "
                   + "host='" + connInfo.host + "' "
                   + "port=" + std::to_string(connInfo.port) + " "
                   + "user='" + connInfo.user + "' "
                   + "password='" + connInfo.password + "'";
        ASSERT_TRUE(conn.open(connString))
            << "Cannot connect to PostgreSQL. Set PGPP_TEST_* env vars.";

        conn.execRaw("DROP TABLE IF EXISTS pgpp_test_table");
        ASSERT_TRUE(conn.execRaw(
            "CREATE TABLE pgpp_test_table ("
            "  id SERIAL PRIMARY KEY,"
            "  name VARCHAR(255),"
            "  score INTEGER DEFAULT 0,"
            "  rating DOUBLE PRECISION DEFAULT 0.0,"
            "  active BOOLEAN DEFAULT true"
            ")"));
    }

    void TearDown() override
    {
        conn.execRaw("DROP TABLE IF EXISTS pgpp_test_table");
        conn.close();
    }
};
