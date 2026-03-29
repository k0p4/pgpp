/*
 * Copyright (C) k0p4 2023-2026
 *
 * This file is part of pgpp — C++ PostgreSQL Connection Pool.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <pgpp/pgpp_connection.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct PgppConnectionInfo {
    std::string dbname;
    std::string host;
    std::string sslmode;
    std::string options;
    std::string user;
    std::string password;
    uint16_t    port { 5432 };
};

struct PgppRequest;

class PgppPool final
{
public:
    PgppPool();
    ~PgppPool();

    PgppPool(const PgppPool&) = delete;
    PgppPool& operator=(const PgppPool&) = delete;

    bool initialize(const PgppConnectionInfo& dbInfo, size_t poolSize = 0);
    void shutdown();
    bool isInitialized() const;
    void prepareStatement(const Statement& statement);

    // Synchronous (blocking)
    template<typename... Ts>
    bool execSync(const std::string& statement, const Ts&... args);

    template<typename RowTuple, typename... Ts>
    std::pair<bool, std::vector<RowTuple>> querySync(const std::string& statement, const Ts&... args);

    // Future-based
    template<typename... Ts>
    std::future<std::optional<bool>> execAsync(const std::string& statement, const Ts&... args);

    template<typename RowTuple, typename... Ts>
    std::future<std::pair<std::optional<bool>, std::vector<RowTuple>>>
        queryAsync(const std::string& statement, const Ts&... args);

    // Callback-based (runs on worker thread)
    template<typename... Ts>
    void exec(const std::string& statement,
              std::function<void(std::optional<bool>)> onDone,
              const Ts&... args);

    template<typename RowTuple, typename... Ts>
    void query(const std::string& statement,
               std::function<void(std::optional<bool>, std::vector<RowTuple>)> onDone,
               const Ts&... args);

    // Raw SQL (non-prepared)
    bool execRawSync(const std::string& sql);
    std::future<std::optional<bool>> execRawAsync(const std::string& sql);

    // Transaction (auto-rollback on exception)
    template<typename F>
    std::future<std::optional<bool>> transaction(F&& work);

    // Public for coroutine awaitables (see pgpp_coroutines.h)
    bool enqueueRaw(std::unique_ptr<PgppRequest> request);

    size_t totalConnections() const;
    size_t freeConnections()  const;
    size_t busyConnections()  const;
    size_t queuedRequests()   const;

private:
    size_t      m_poolSize { std::thread::hardware_concurrency() };
    std::string m_connectionString;

    std::vector<std::unique_ptr<PgppConnection>> m_connections;
    std::queue<std::unique_ptr<PgppRequest>>     m_requestQueue;

    mutable std::mutex      m_queueMutex;
    std::condition_variable m_requestQueued;

    std::vector<std::thread>  m_workerThreads;
    std::atomic<bool>         m_shuttingDown { false };
    std::atomic<bool>         m_initialized  { false };
    std::atomic<size_t>       m_busyWorkers  { 0 };

    std::vector<Statement> m_preparedStatements;

    bool createConnections(const PgppConnectionInfo& dbInfo);
    void startWorkerThreads();
    void stopWorkerThreads();
    void workerLoop(size_t connIdx);
    std::string buildConnectionString(const PgppConnectionInfo& dbInfo) const;
    void prepareStatementsOnConnection(PgppConnection* conn);
};

struct PgppRequest {
    std::function<void(PgppConnection*)> task;
};

// ── Template implementations ─────────────────────────────────────────────────

template<typename... Ts>
bool PgppPool::execSync(const std::string& statement, const Ts&... args)
{
    auto result = execAsync(statement, args...).get();
    return result.has_value() && result.value();
}

template<typename RowTuple, typename... Ts>
std::pair<bool, std::vector<RowTuple>> PgppPool::querySync(const std::string& statement, const Ts&... args)
{
    auto [ok, rows] = queryAsync<RowTuple>(statement, args...).get();
    return { ok.has_value() && ok.value(), std::move(rows) };
}

template<typename... Ts>
std::future<std::optional<bool>> PgppPool::execAsync(const std::string& statement, const Ts&... args)
{
    auto promise = std::make_shared<std::promise<std::optional<bool>>>();
    auto future = promise->get_future();
    if (m_shuttingDown.load()) [[unlikely]] { promise->set_value(std::nullopt); return future; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [statement, promise, args...](PgppConnection* conn) {
        if (!conn) { promise->set_value(std::nullopt); return; }
        promise->set_value(conn->execPrepared(statement, args...));
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] promise->set_value(std::nullopt);
    return future;
}

template<typename RowTuple, typename... Ts>
std::future<std::pair<std::optional<bool>, std::vector<RowTuple>>>
PgppPool::queryAsync(const std::string& statement, const Ts&... args)
{
    using Result = std::pair<std::optional<bool>, std::vector<RowTuple>>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    if (m_shuttingDown.load()) [[unlikely]] { promise->set_value({std::nullopt, {}}); return future; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [statement, promise, args...](PgppConnection* conn) {
        if (!conn) { promise->set_value({std::nullopt, {}}); return; }
        std::vector<RowTuple> rows;
        bool ok = conn->execPrepared(statement, rows, args...);
        promise->set_value({ok, std::move(rows)});
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] promise->set_value({std::nullopt, {}});
    return future;
}

template<typename... Ts>
void PgppPool::exec(const std::string& statement,
                    std::function<void(std::optional<bool>)> onDone,
                    const Ts&... args)
{
    if (m_shuttingDown.load()) [[unlikely]] { onDone(std::nullopt); return; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [statement, onDone = std::move(onDone), args...](PgppConnection* conn) mutable {
        if (!conn) { onDone(std::nullopt); return; }
        onDone(conn->execPrepared(statement, args...));
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] onDone(std::nullopt);
}

template<typename RowTuple, typename... Ts>
void PgppPool::query(const std::string& statement,
                     std::function<void(std::optional<bool>, std::vector<RowTuple>)> onDone,
                     const Ts&... args)
{
    if (m_shuttingDown.load()) [[unlikely]] { onDone(std::nullopt, {}); return; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [statement, onDone = std::move(onDone), args...](PgppConnection* conn) mutable {
        if (!conn) { onDone(std::nullopt, {}); return; }
        std::vector<RowTuple> rows;
        bool ok = conn->execPrepared(statement, rows, args...);
        onDone(ok, std::move(rows));
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] onDone(std::nullopt, {});
}

template<typename F>
std::future<std::optional<bool>> PgppPool::transaction(F&& work)
{
    auto promise = std::make_shared<std::promise<std::optional<bool>>>();
    auto future = promise->get_future();
    if (m_shuttingDown.load()) [[unlikely]] { promise->set_value(std::nullopt); return future; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [promise, work = std::forward<F>(work)](PgppConnection* conn) mutable {
        if (!conn) { promise->set_value(std::nullopt); return; }

        PGresult* res = PQexec(conn->connection(), "BEGIN");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); promise->set_value(false); return; }
        PQclear(res);

        try {
            work(*conn);
            res = PQexec(conn->connection(), "COMMIT");
            bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
            PQclear(res);
            promise->set_value(ok);
        } catch (...) {
            res = PQexec(conn->connection(), "ROLLBACK");
            PQclear(res);
            promise->set_value(false);
        }
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] promise->set_value(std::nullopt);
    return future;
}
