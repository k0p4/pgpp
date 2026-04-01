#pragma once

#include <pgpp/pgpp.h>
#include <cstdlib>
#include <string>

namespace pgpp_test {

// Database credentials
constexpr const char* DB_HOST     = "127.0.0.1";
constexpr uint16_t    DB_PORT     = 15432;  // Non-standard port to avoid conflicts
constexpr const char* DB_USER     = "postgres";
constexpr const char* DB_PASSWORD = "pgpp_test_pass";
constexpr const char* DB_NAME     = "pgpp_test";

// Docker settings
constexpr const char* DOCKER_CONTAINER_NAME = "pgpp-test-pg-7f3a";
constexpr const char* DOCKER_IMAGE          = "postgres:16-alpine";

// Timeouts
constexpr int DOCKER_STARTUP_TIMEOUT_SEC  = 30;
constexpr int DOCKER_SHUTDOWN_TIMEOUT_SEC = 15;
constexpr int DOCKER_POLL_INTERVAL_MS     = 500;
constexpr int QUERY_TIMEOUT_MS            = 5000;
constexpr int SLOW_QUERY_TIMEOUT_MS       = 30000;

inline PgppConnectionInfo getTestConnectionInfo()
{
    PgppConnectionInfo info;
    auto env = [](const char* name, const char* fallback) -> std::string {
        const char* val = std::getenv(name);
        return val ? val : fallback;
    };
    info.host     = env("PGPP_TEST_HOST",     DB_HOST);
    info.port     = static_cast<uint16_t>(std::stoi(env("PGPP_TEST_PORT", std::to_string(DB_PORT).c_str())));
    info.user     = env("PGPP_TEST_USER",     DB_USER);
    info.password = env("PGPP_TEST_PASSWORD", DB_PASSWORD);
    info.dbname   = env("PGPP_TEST_DBNAME",   DB_NAME);
    info.sslmode  = "prefer";
    return info;
}

} // namespace pgpp_test
