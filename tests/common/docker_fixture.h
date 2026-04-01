#pragma once

#include "test_config.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>

namespace pgpp_test {

class DockerPostgresEnvironment : public ::testing::Environment {
public:
    void SetUp() override
    {
        // Check if Docker management should be skipped
        const char* skipDocker = std::getenv("PGPP_SKIP_DOCKER");
        if (skipDocker && std::string(skipDocker) == "1") {
            std::cout << "[Docker] PGPP_SKIP_DOCKER=1, skipping Docker management\n";
            return;
        }

        std::cout << "[Docker] Setting up PostgreSQL container...\n";

        // Stop existing container if running
        if (isContainerRunning()) {
            std::cout << "[Docker] Container already running, stopping...\n";
            stopContainer();
        }

        // Remove stopped container if exists
        exec("docker rm -f " + std::string(DOCKER_CONTAINER_NAME) + devnull());

        // Start fresh container
        std::string runCmd =
            "docker run -d"
            " --name " + std::string(DOCKER_CONTAINER_NAME) +
            " -e POSTGRES_PASSWORD=" + std::string(DB_PASSWORD) +
            " -e POSTGRES_DB=" + std::string(DB_NAME) +
            " -e POSTGRES_USER=" + std::string(DB_USER) +
            " -p " + std::to_string(DB_PORT) + ":5432"
            " " + std::string(DOCKER_IMAGE);

        std::cout << "[Docker] Starting container: " << DOCKER_CONTAINER_NAME << "\n";
        int result = exec(runCmd);
        if (result != 0) {
            FAIL() << "Failed to start Docker container. Is Docker running?";
            return;
        }

        m_dockerManaged = true;

        // Wait for PostgreSQL to be ready
        if (!waitForReady()) {
            FAIL() << "PostgreSQL did not become ready within " << DOCKER_STARTUP_TIMEOUT_SEC << " seconds";
        }

        std::cout << "[Docker] PostgreSQL is ready on port " << DB_PORT << "\n";
    }

    void TearDown() override
    {
        if (!m_dockerManaged) {
            return;
        }

        std::cout << "[Docker] Stopping PostgreSQL container...\n";
        stopContainer();

        // Remove container
        exec("docker rm -f " + std::string(DOCKER_CONTAINER_NAME) + devnull());

        std::cout << "[Docker] Container cleanup complete\n";
    }

private:
    bool m_dockerManaged {};

    static std::string devnull()
    {
#ifdef _WIN32
        return " >nul 2>&1";
#else
        return " >/dev/null 2>&1";
#endif
    }

    int exec(const std::string& cmd)
    {
        return std::system(cmd.c_str());
    }

    bool isContainerRunning()
    {
#ifdef _WIN32
        std::string cmd =
            "docker inspect --format \"{{.State.Running}}\" " +
            std::string(DOCKER_CONTAINER_NAME) + " 2>nul | findstr true >nul 2>&1";
#else
        std::string cmd =
            "docker inspect --format '{{.State.Running}}' " +
            std::string(DOCKER_CONTAINER_NAME) + " 2>/dev/null | grep -q true";
#endif
        return exec(cmd) == 0;
    }

    bool waitForContainerGone(int timeoutSec)
    {
        auto start = std::chrono::steady_clock::now();
        while (isContainerRunning()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeoutSec) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(DOCKER_POLL_INTERVAL_MS));
        }
        return true;
    }

    bool waitForReady()
    {
        std::string cmd =
            "docker exec " + std::string(DOCKER_CONTAINER_NAME) +
            " pg_isready -h localhost -U " + std::string(DB_USER) + devnull();

        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (exec(cmd) == 0) {
                return true;
            }

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= DOCKER_STARTUP_TIMEOUT_SEC) {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(DOCKER_POLL_INTERVAL_MS));
        }
    }

    void stopContainer()
    {
        // Try graceful stop
        exec("docker stop " + std::string(DOCKER_CONTAINER_NAME) + devnull());

        if (waitForContainerGone(DOCKER_SHUTDOWN_TIMEOUT_SEC)) {
            return;
        }

        // Force kill
        std::cout << "[Docker] Graceful stop failed, killing container...\n";
        exec("docker kill " + std::string(DOCKER_CONTAINER_NAME) + devnull());

        if (waitForContainerGone(DOCKER_SHUTDOWN_TIMEOUT_SEC)) {
            return;
        }

        FAIL() << "Failed to stop Docker container " << DOCKER_CONTAINER_NAME;
    }
};

} // namespace pgpp_test
