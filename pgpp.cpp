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

#include "pgpp.h"
#include "pgpp_log.h"
PGPP_DEFINE_LOG_MODULE(PgppPool)

static std::string escapeConnValue(const std::string& v)
{
    std::string out;
    out.reserve(v.size() + 4);
    out += '\'';
    for (char c : v) {
        if (c == '\'') out += '\'';
        out += c;
    }
    out += '\'';
    return out;
}

std::string PgppPool::buildConnectionString(const PgppConnectionInfo& dbInfo) const
{
    std::string connStr;

    if (!dbInfo.host.empty())
        connStr += "host=" + escapeConnValue(dbInfo.host) + " ";
    else
        PGPP_LOGW << "Host is empty";

    if (dbInfo.port != 0)
        connStr += "port=" + std::to_string(dbInfo.port) + " ";

    if (!dbInfo.dbname.empty())
        connStr += "dbname=" + escapeConnValue(dbInfo.dbname) + " ";
    else {
        PGPP_LOGE << "Database name is empty";
        return "";
    }

    if (!dbInfo.user.empty()) {
        connStr += "user=" + escapeConnValue(dbInfo.user) + " ";
        if (!dbInfo.password.empty())
            connStr += "password=" + escapeConnValue(dbInfo.password) + " ";
    }

    if (!dbInfo.sslmode.empty())
        connStr += "sslmode=" + escapeConnValue(dbInfo.sslmode) + " ";

    PGPP_LOGD << "Connection: host=\"" << dbInfo.host
              << "\" port=" << dbInfo.port
              << " dbname=\"" << dbInfo.dbname
              << "\" user=\"" << dbInfo.user << "\"";

    return connStr;
}

bool PgppPool::createConnections(const PgppConnectionInfo& dbInfo)
{
    m_connectionString = buildConnectionString(dbInfo);
    if (m_connectionString.empty()) {
        PGPP_LOGE << "Failed to build connection string";
        return false;
    }

    m_connections.reserve(m_poolSize);
    for (size_t i = 0; i < m_poolSize; ++i) {
        auto conn = std::make_unique<PgppConnection>();
        if (!conn->open(m_connectionString)) [[unlikely]] {
            PGPP_LOGE << "Failed to create connection " << i << ": " << conn->lastError();
            m_connections.clear();
            return false;
        }
        prepareStatementsOnConnection(conn.get());
        m_connections.push_back(std::move(conn));
    }

    PGPP_LOGD << "Created " << m_poolSize << " connections";
    return true;
}

void PgppPool::prepareStatementsOnConnection(PgppConnection* conn)
{
    for (const auto& stmt : m_preparedStatements) {
        if (!conn->prepare(stmt)) [[unlikely]]
            PGPP_LOGW << "Failed to prepare: " << stmt.statementName;
    }
}

void PgppPool::startWorkerThreads()
{
    for (size_t i = 0; i < m_poolSize; ++i)
        m_workerThreads.emplace_back(&PgppPool::workerLoop, this, i);
    PGPP_LOGD << "Started " << m_poolSize << " worker threads";
}

void PgppPool::stopWorkerThreads()
{
    if (m_shuttingDown.exchange(true)) return;
    m_requestQueued.notify_all();
    for (auto& w : m_workerThreads)
        if (w.joinable()) w.join();
    m_workerThreads.clear();
}

void PgppPool::workerLoop(size_t connIdx)
{
    PgppConnection* conn = m_connections[connIdx].get();

    while (!m_shuttingDown.load()) {
        std::unique_ptr<PgppRequest> request;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_requestQueued.wait(lock, [this] {
                return !m_requestQueue.empty() || m_shuttingDown.load();
            });
            if (m_shuttingDown.load()) [[unlikely]] break;
            if (!m_requestQueue.empty()) [[likely]] {
                request = std::move(m_requestQueue.front());
                m_requestQueue.pop();
            }
        }

        if (!request) [[unlikely]] continue;

        if (!conn->isOpen()) [[unlikely]] {
            PGPP_LOGW << "Connection " << connIdx << " lost, reconnecting...";
            conn->reset();
            if (conn->isOpen()) {
                prepareStatementsOnConnection(conn);
                PGPP_LOGD << "Connection " << connIdx << " restored";
            } else {
                PGPP_LOGE << "Connection " << connIdx << " reconnect failed";
                try { request->task(nullptr); } catch (...) {}
                continue;
            }
        }

        m_busyWorkers.fetch_add(1, std::memory_order_relaxed);
        try {
            request->task(conn);
        } catch (const std::exception& e) {
            PGPP_LOGE << "Request exception: " << e.what();
        } catch (...) {
            PGPP_LOGE << "Unknown request exception";
        }
        m_busyWorkers.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool PgppPool::enqueueRaw(std::unique_ptr<PgppRequest> request)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_shuttingDown.load()) [[unlikely]] return false;
    m_requestQueue.push(std::move(request));
    m_requestQueued.notify_one();
    return true;
}

PgppPool::PgppPool()
{
    if (m_poolSize == 0) m_poolSize = 16;
}

PgppPool::~PgppPool()
{
    shutdown();
}

bool PgppPool::initialize(const PgppConnectionInfo& dbInfo, size_t poolSize)
{
    if (m_initialized.exchange(true)) [[unlikely]] {
        PGPP_LOGW << "Already initialized";
        return true;
    }

    if (poolSize > 0) [[likely]] m_poolSize = poolSize;

    if (!createConnections(dbInfo)) [[unlikely]] {
        m_initialized = false;
        return false;
    }

    startWorkerThreads();
    PGPP_LOGD << "Pool initialized with " << m_poolSize << " connections";
    return true;
}

void PgppPool::shutdown()
{
    if (!m_initialized.exchange(false)) [[unlikely]] return;

    stopWorkerThreads();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_requestQueue.empty()) {
            auto req = std::move(m_requestQueue.front());
            m_requestQueue.pop();
            try { req->task(nullptr); } catch (...) {}
        }
    }

    m_connections.clear();
    PGPP_LOGD << "Pool shut down";
}

bool PgppPool::isInitialized() const { return m_initialized.load(); }

void PgppPool::prepareStatement(const Statement& statement)
{
    m_preparedStatements.push_back(statement);

    if (m_workerThreads.empty()) {
        for (auto& conn : m_connections) {
            if (!conn->prepare(statement)) [[unlikely]]
                PGPP_LOGW << "Failed to prepare: " << statement.statementName;
        }
    } else {
        for (size_t i = 0; i < m_poolSize; ++i) {
            auto req = std::make_unique<PgppRequest>();
            req->task = [stmt = statement](PgppConnection* conn) {
                if (conn) conn->prepare(stmt);
            };
            enqueueRaw(std::move(req));
        }
    }
}

bool PgppPool::execRawSync(const std::string& sql)
{
    auto result = execRawAsync(sql).get();
    return result.has_value() && result.value();
}

std::future<std::optional<bool>> PgppPool::execRawAsync(const std::string& sql)
{
    auto promise = std::make_shared<std::promise<std::optional<bool>>>();
    auto future = promise->get_future();
    if (m_shuttingDown.load()) [[unlikely]] { promise->set_value(std::nullopt); return future; }

    auto request = std::make_unique<PgppRequest>();
    request->task = [sql, promise](PgppConnection* conn) {
        if (!conn) { promise->set_value(std::nullopt); return; }
        promise->set_value(conn->execRaw(sql));
    };
    if (!enqueueRaw(std::move(request))) [[unlikely]] promise->set_value(std::nullopt);
    return future;
}

size_t PgppPool::totalConnections() const { return m_connections.size(); }

size_t PgppPool::freeConnections() const
{
    const size_t busy = m_busyWorkers.load(std::memory_order_relaxed);
    const size_t total = m_connections.size();
    return (busy <= total) ? (total - busy) : 0;
}

size_t PgppPool::busyConnections() const { return m_busyWorkers.load(std::memory_order_relaxed); }

size_t PgppPool::queuedRequests() const
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_requestQueue.size();
}
