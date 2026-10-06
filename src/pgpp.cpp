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

#include <pgpp/pgpp.h>
#include "pgpp_log.h"

#include <chrono>
#include <thread>

PGPP_DEFINE_LOG_MODULE(PgppPool)

static std::string escapeConnValue(const std::string& v)
{
    std::string out;
    out.reserve(v.size() + 4);
    out += '\'';
    for (char c : v) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    out += '\'';
    return out;
}

std::string PgppPool::buildConnectionString(const PgppConnectionInfo& dbInfo) const noexcept
{
    try {
        std::string connStr;

        if (!dbInfo.host.empty()) {
            connStr += "host=" + escapeConnValue(dbInfo.host) + " ";
        } else {
            PGPP_LOGW << "Host is empty";
        }

        if (dbInfo.port != 0) {
            connStr += "port=" + std::to_string(dbInfo.port) + " ";
        }

        if (!dbInfo.dbname.empty()) {
            connStr += "dbname=" + escapeConnValue(dbInfo.dbname) + " ";
        } else {
            PGPP_LOGE << "Database name is empty";
            return "";
        }

        if (!dbInfo.user.empty()) {
            connStr += "user=" + escapeConnValue(dbInfo.user) + " ";
            if (!dbInfo.password.empty()) {
                connStr += "password=" + escapeConnValue(dbInfo.password) + " ";
            }
        }

        if (!dbInfo.sslmode.empty()) {
            connStr += "sslmode=" + escapeConnValue(dbInfo.sslmode) + " ";
        }

        if (!dbInfo.options.empty()) {
            connStr += "options=" + escapeConnValue(dbInfo.options) + " ";
        }

        PGPP_LOGD << "Connection: host=\"" << dbInfo.host
                  << "\" port=" << dbInfo.port
                  << " dbname=\"" << dbInfo.dbname
                  << "\" user=\"" << dbInfo.user << "\"";

        return connStr;
    } catch (...) {
        PGPP_LOGE << "Failed to build connection string (allocation)";
        return "";
    }
}

bool PgppPool::createConnections(const PgppConnectionInfo& dbInfo) noexcept
{
    try {
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
    } catch (...) {
        PGPP_LOGE << "Failed to create connections (allocation)";
        m_connections.clear();
        return false;
    }
}

void PgppPool::prepareStatementsOnConnection(PgppConnection* conn) noexcept
{
    try {
        std::vector<Statement> stmts;
        {
            std::lock_guard<std::mutex> lock(m_stmtMutex);
            stmts = m_preparedStatements;
        }
        for (const auto& stmt : stmts) {
            if (!conn->prepare(stmt)) [[unlikely]] {
                PGPP_LOGW << "Failed to prepare: " << stmt.statementName;
            }
        }
    } catch (...) {
        PGPP_LOGE << "Failed to re-prepare statements on a connection (allocation or mutex error)";
    }
}

bool PgppPool::startWorkerThreads() noexcept
{
    // Capture the statement version *before* the threads exist: a prepareStatement()
    // that lands between thread creation and the worker's first instruction bumps the
    // version past this value, so the worker still notices it and re-prepares.
    const uint32_t stmtVersion = m_stmtVersion.load(std::memory_order_acquire);
    try {
        m_workerThreads.reserve(m_poolSize);
        for (size_t i = 0; i < m_poolSize; ++i) {
            m_workerThreads.emplace_back(&PgppPool::workerLoop, this, i, stmtVersion);
        }
        PGPP_LOGD << "Started " << m_poolSize << " worker threads";
        return true;
    } catch (...) {
        // std::system_error from thread creation, or allocation: the caller stops
        // whatever did start.
        PGPP_LOGE << "Failed to start worker threads";
        return false;
    }
}

void PgppPool::stopWorkerThreads() noexcept
{
    try {
        // Flip the flag under m_queueMutex: the workers test it inside their wait
        // predicate, so a flip + notify between that test and the actual block would
        // otherwise be lost and join() below would never return.
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_shuttingDown.exchange(true)) {
            return;
        }
    } catch (...) {
        // Locking failed: set the flag anyway; the workers also test it unlocked.
        PGPP_LOGE << "Failed to lock the queue while stopping workers";
        if (m_shuttingDown.exchange(true)) {
            return;
        }
    }
    m_requestQueued.notify_all();

    for (auto& worker : m_workerThreads) {
        try {
            if (worker.joinable()) {
                worker.join();
            }
        } catch (...) {
            // join() can only fail for a thread that is not joinable or is the
            // current thread; a thread left joinable would terminate the process.
            PGPP_LOGE << "Failed to join a worker thread";
            if (worker.joinable()) {
                worker.detach();
            }
        }
    }
    m_workerThreads.clear();
}

void PgppPool::workerLoop(size_t connIdx, uint32_t stmtVersion) noexcept
{
    PgppConnection* conn = m_connections[connIdx].get();
    uint32_t localStmtVersion = stmtVersion;

    while (!m_shuttingDown.load()) {
        std::unique_ptr<PgppRequest> request;
        try {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_requestQueued.wait(lock, [this, localStmtVersion] {
                return !m_requestQueue.empty() || m_shuttingDown.load()
                    || m_stmtVersion.load(std::memory_order_acquire) != localStmtVersion;
            });
            if (m_shuttingDown.load()) [[unlikely]] {
                break;
            }
            if (!m_requestQueue.empty()) [[likely]] {
                request = std::move(m_requestQueue.front());
                m_requestQueue.pop();
            }
        } catch (...) {
            // std::system_error from the mutex or condition variable: nothing was
            // dequeued; try again (the loop condition still honours shutdown).
            // Back off first: a persistent failure must not become a busy spin.
            PGPP_LOGE << "Worker " << connIdx << ": failed to wait on the request queue";
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } catch (...) {
            }
            continue;
        }

        // Check if new statements need preparing on this connection
        uint32_t currentVersion = m_stmtVersion.load(std::memory_order_acquire);
        if (currentVersion != localStmtVersion) {
            localStmtVersion = currentVersion;
            prepareStatementsOnConnection(conn);
        }

        if (!request) [[unlikely]] {
            continue;
        }

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

bool PgppPool::enqueueRaw(std::unique_ptr<PgppRequest> request) noexcept
{
    if (!m_initialized.load()) [[unlikely]] {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_shuttingDown.load()) [[unlikely]] {
            return false;
        }
        m_requestQueue.push(std::move(request));
        m_requestQueued.notify_one();
        return true;
    } catch (...) {
        PGPP_LOGE << "Failed to enqueue a request (allocation or mutex error)";
        return false;
    }
}

PgppPool::PgppPool()
{
    if (m_poolSize == 0) m_poolSize = 16;
}

PgppPool::~PgppPool()
{
    shutdown();
}

bool PgppPool::initialize(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept
{
    if (m_initialized.exchange(true)) [[unlikely]] {
        PGPP_LOGW << "Already initialized";
        return true;
    }

    m_shuttingDown.store(false);

    if (poolSize > 0) [[likely]] {
        m_poolSize = poolSize;
    }

    if (!createConnections(dbInfo) || !startWorkerThreads()) [[unlikely]] {
        stopWorkerThreads();        // joins whatever did start
        m_connections.clear();
        m_initialized = false;
        return false;
    }

    PGPP_LOGD << "Pool initialized with " << m_poolSize << " connections";
    return true;
}

void PgppPool::shutdown() noexcept
{
    if (!m_initialized.exchange(false)) [[unlikely]] {
        return;
    }

    stopWorkerThreads();

    // Drain pending requests one at a time, outside the lock: task(nullptr)
    // resumes coroutines which may call enqueueRaw. No intermediate container,
    // so nothing here can fail for lack of memory.
    while (true) {
        std::unique_ptr<PgppRequest> request;
        try {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_requestQueue.empty()) {
                break;
            }
            request = std::move(m_requestQueue.front());
            m_requestQueue.pop();
        } catch (...) {
            PGPP_LOGE << "Failed to lock the queue while draining; pending requests dropped";
            break;
        }
        try { request->task(nullptr); } catch (...) {}
    }

    m_connections.clear();
    PGPP_LOGD << "Pool shut down";
}

bool PgppPool::isInitialized() const noexcept { return m_initialized.load(); }

void PgppPool::prepareStatement(const Statement& statement) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_stmtMutex);
        m_preparedStatements.push_back(statement);
    } catch (...) {
        PGPP_LOGE << "Failed to register statement " << statement.statementName << " (allocation or mutex error)";
        return;
    }

    if (m_workerThreads.empty()) {
        for (auto& conn : m_connections) {
            if (!conn->prepare(statement)) [[unlikely]] {
                PGPP_LOGW << "Failed to prepare: " << statement.statementName;
            }
        }
    } else {
        // Wake all workers — each will check m_stmtVersion and re-prepare.
        m_stmtVersion.fetch_add(1, std::memory_order_release);
        m_requestQueued.notify_all();
    }
}

bool PgppPool::execRawSync(const std::string& sql) noexcept
{
    auto future = execRawAsync(sql);
    if (!future.valid()) {
        return false;
    }

    try {
        const auto result = future.get();
        return result.has_value() && result.value();
    } catch (...) {
        return false;
    }
}

std::future<std::optional<bool>> PgppPool::execRawAsync(const std::string& sql) noexcept
{
    using Result = std::optional<bool>;
    if (m_shuttingDown.load()) [[unlikely]] {
        return Internal::resolvedFuture<Result>(std::nullopt);
    }

    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        auto request = std::make_unique<PgppRequest>();
        request->task = [sql, promise](PgppConnection* conn) {
            if (!conn) {
                promise->set_value(std::nullopt);
            } else {
                promise->set_value(conn->execRaw(sql));
            }
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            promise->set_value(std::nullopt);
        }
        return future;
    } catch (...) {
        PGPP_LOGE << "Failed to build a raw request (allocation)";
        return Internal::resolvedFuture<Result>(std::nullopt);
    }
}

size_t PgppPool::totalConnections() const noexcept { return m_connections.size(); }

size_t PgppPool::freeConnections() const noexcept
{
    const size_t busy = m_busyWorkers.load(std::memory_order_relaxed);
    const size_t total = m_connections.size();
    return (busy <= total) ? (total - busy) : 0;
}

size_t PgppPool::busyConnections() const noexcept { return m_busyWorkers.load(std::memory_order_relaxed); }

size_t PgppPool::queuedRequests() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_requestQueue.size();
    } catch (...) {
        return 0;
    }
}
