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

// ── Worker identity ──────────────────────────────────────────────────────────
//
// Each worker thread owns one WorkerContext for its lifetime and publishes it
// through t_worker. User code that runs on a worker (callbacks, transaction
// work, resumed coroutines) can therefore be recognised, and a request it issues
// runs inline on that worker's connection instead of being queued: the
// connection is idle while user code runs, and blocking on a queued request
// from a worker would wait for a worker that may be this very thread
// (REQ-PGPP-067).

struct PgppPool::WorkerContext {
    PgppPool*       pool;
    PgppConnection* conn;
    int             depth             { 0 };      // nesting of inline requests
    bool            shutdownRequested { false };  // shutdown() was called on this worker
};

thread_local PgppPool::WorkerContext* PgppPool::t_worker = nullptr;

bool PgppPool::onOwnWorker() const noexcept
{
    return t_worker != nullptr && t_worker->pool == this;
}

int PgppPool::inlineDepth() noexcept
{
    return t_worker != nullptr ? t_worker->depth : 0;
}

// ── Connection string ────────────────────────────────────────────────────────

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

// ── Connections and statements ───────────────────────────────────────────────

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
        m_connectionCount.store(m_connections.size(), std::memory_order_release);

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

// ── Worker threads ───────────────────────────────────────────────────────────

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

void PgppPool::signalShutdown() noexcept
{
    try {
        // Flip the flag under m_queueMutex: the workers test it inside their wait
        // predicate, so a flip + notify between that test and the actual block would
        // otherwise be lost and join() would never return (REQ-PGPP-060).
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shuttingDown.store(true);
    } catch (...) {
        // Locking failed: set the flag anyway; the workers also test it unlocked.
        PGPP_LOGE << "Failed to lock the queue while stopping workers";
        m_shuttingDown.store(true);
    }
    m_requestQueued.notify_all();
}

void PgppPool::joinWorkers(std::thread::id self) noexcept
{
    for (auto& worker : m_workerThreads) {
        if (worker.get_id() == self) {
            // A thread cannot join itself: the worker running the deferred teardown
            // keeps its handle here for the next non-worker shutdown() to join
            // (REQ-PGPP-069). Never detached: the destructor must be able to wait.
            joinSelfShutdownThread();
            m_selfShutdownThread = std::move(worker);
            continue;
        }
        try {
            if (worker.joinable()) {
                worker.join();
            }
        } catch (...) {
            // Cannot happen for another thread that is joinable; a thread left
            // joinable would terminate the process, so detach is the lesser evil.
            PGPP_LOGE << "Failed to join a worker thread";
            if (worker.joinable()) {
                worker.detach();
            }
        }
    }
    m_workerThreads.clear();
}

void PgppPool::joinSelfShutdownThread() noexcept
{
    try {
        if (m_selfShutdownThread.joinable()) {
            m_selfShutdownThread.join();
        }
    } catch (...) {
        PGPP_LOGE << "Failed to join the worker that ran shutdown()";
        if (m_selfShutdownThread.joinable()) {
            m_selfShutdownThread.detach();
        }
    }
}

void PgppPool::drainQueue() noexcept
{
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
}

void PgppPool::teardown(std::thread::id self) noexcept
{
    signalShutdown();
    joinWorkers(self);
    drainQueue();
    m_connectionCount.store(0, std::memory_order_release);
    m_connections.clear();
    PGPP_LOGD << "Pool shut down";
}

void PgppPool::serveRequest(WorkerContext& ctx, PgppRequest& request) noexcept
{
    PgppConnection* conn = ctx.conn;

    if (!conn->isOpen()) [[unlikely]] {
        PGPP_LOGW << "Connection lost, reconnecting...";
        conn->reset();
        if (conn->isOpen()) {
            prepareStatementsOnConnection(conn);
            PGPP_LOGD << "Connection restored";
        } else {
            PGPP_LOGE << "Reconnect failed";
            try { request.task(nullptr); } catch (...) {}
            return;
        }
    }

    // Inline (nested) requests run inside the outer task's busy period.
    if (ctx.depth == 0) {
        m_busyWorkers.fetch_add(1, std::memory_order_relaxed);
    }
    ++ctx.depth;
    try {
        request.task(conn);
    } catch (const std::exception& e) {
        PGPP_LOGE << "Request exception: " << e.what();
    } catch (...) {
        PGPP_LOGE << "Unknown request exception";
    }
    --ctx.depth;
    if (ctx.depth == 0) {
        m_busyWorkers.fetch_sub(1, std::memory_order_relaxed);
    }
}

void PgppPool::workerLoop(size_t connIdx, uint32_t stmtVersion) noexcept
{
    WorkerContext ctx { this, m_connections[connIdx].get() };
    t_worker = &ctx;
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
            prepareStatementsOnConnection(ctx.conn);
        }

        if (!request) [[unlikely]] {
            continue;
        }

        serveRequest(ctx, *request);
        request.reset();

        if (ctx.shutdownRequested) [[unlikely]] {
            // shutdown() was called from this task's user code (REQ-PGPP-069).
            finishShutdownOnWorker();
            break;
        }
    }

    t_worker = nullptr;
}

bool PgppPool::submit(std::unique_ptr<PgppRequest> request) noexcept
{
    if (onOwnWorker()) {
        serveRequest(*t_worker, *request);
        return true;
    } else {
        return enqueueRaw(std::move(request));
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

// ── Lifecycle ────────────────────────────────────────────────────────────────

PgppPool::PgppPool()
{
    if (m_poolSize == 0) m_poolSize = 16;
}

PgppPool::~PgppPool()
{
    shutdown();
}

bool PgppPool::initializeLocked(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept
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
        teardown(std::thread::id {});   // joins whatever did start
        m_initialized = false;
        return false;
    }

    PGPP_LOGD << "Pool initialized with " << m_poolSize << " connections";
    return true;
}

bool PgppPool::initialize(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept
{
    if (onOwnWorker()) {
        // A worker exists, so the pool is initialized; taking m_lifecycleMutex here
        // could wait on a shutdown() that is waiting on this worker.
        PGPP_LOGW << "initialize() called from a worker thread";
        return m_initialized.load();
    }

    try {
        std::unique_lock<std::mutex> lock(m_lifecycleMutex);
        m_lifecycleChanged.wait(lock, [this] { return !m_deferredShutdown; });
        joinSelfShutdownThread();
        return initializeLocked(dbInfo, poolSize);
    } catch (...) {
        PGPP_LOGE << "initialize: lifecycle lock failed";
        return initializeLocked(dbInfo, poolSize);
    }
}

void PgppPool::shutdownLocked() noexcept
{
    joinSelfShutdownThread();
    if (!m_initialized.exchange(false)) [[unlikely]] {
        return;
    }
    teardown(std::thread::id {});
}

void PgppPool::shutdown() noexcept
{
    if (onOwnWorker()) {
        requestShutdownFromWorker();
        return;
    }

    try {
        std::unique_lock<std::mutex> lock(m_lifecycleMutex);
        // A worker that called shutdown() on itself finishes the teardown after its
        // task; wait for that rather than racing it (REQ-PGPP-069).
        m_lifecycleChanged.wait(lock, [this] { return !m_deferredShutdown; });
        shutdownLocked();
    } catch (...) {
        PGPP_LOGE << "shutdown: lifecycle lock failed";
        shutdownLocked();
    }
}

void PgppPool::requestShutdownFromWorker() noexcept
{
    // If a shutdown is already in progress elsewhere, it has exchanged the flag and
    // is (or will be) joining this worker: nothing to do but finish the task.
    if (!m_initialized.exchange(false)) {
        return;
    }
    PGPP_LOGW << "shutdown() called from a worker thread: teardown deferred until its task returns";
    try {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        m_deferredShutdown = true;
    } catch (...) {
        m_deferredShutdown = true;
    }
    signalShutdown();   // other workers exit, new requests are refused
    t_worker->shutdownRequested = true;
}

void PgppPool::finishShutdownOnWorker() noexcept
{
    // Runs on the worker after the task that called shutdown() returned. Holds
    // m_lifecycleMutex so a concurrent non-worker shutdown()/initialize() waits.
    // After the final unlock this thread touches nothing of the pool: the
    // waiting thread joins its handle (kept in joinWorkers) and may destroy
    // the pool right after.
    try {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        teardown(std::this_thread::get_id());
        m_deferredShutdown = false;
        m_lifecycleChanged.notify_all();
    } catch (...) {
        PGPP_LOGE << "deferred shutdown: lifecycle lock failed";
        teardown(std::this_thread::get_id());
        m_deferredShutdown = false;
        m_lifecycleChanged.notify_all();
    }
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

    if (onOwnWorker()) {
        // Workers exist, so only the version bump applies; m_lifecycleMutex may be
        // held by a shutdown() that is waiting on this very worker.
        m_stmtVersion.fetch_add(1, std::memory_order_release);
        m_requestQueued.notify_all();
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
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
    } catch (...) {
        PGPP_LOGE << "prepareStatement: lifecycle lock failed; statement applies on the next re-prepare";
        m_stmtVersion.fetch_add(1, std::memory_order_release);
        m_requestQueued.notify_all();
    }
}

// ── Raw SQL ──────────────────────────────────────────────────────────────────

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
        if (!submit(std::move(request))) [[unlikely]] {
            promise->set_value(std::nullopt);
        }
        return future;
    } catch (...) {
        PGPP_LOGE << "Failed to build a raw request (allocation)";
        return Internal::resolvedFuture<Result>(std::nullopt);
    }
}

// ── Statistics ───────────────────────────────────────────────────────────────

size_t PgppPool::totalConnections() const noexcept
{
    return m_connectionCount.load(std::memory_order_acquire);
}

size_t PgppPool::freeConnections() const noexcept
{
    const size_t busy  = m_busyWorkers.load(std::memory_order_relaxed);
    const size_t total = m_connectionCount.load(std::memory_order_acquire);
    return (busy <= total) ? (total - busy) : 0;
}

size_t PgppPool::busyConnections() const noexcept
{
    return m_busyWorkers.load(std::memory_order_relaxed);
}

size_t PgppPool::queuedRequests() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_requestQueue.size();
    } catch (...) {
        return 0;
    }
}
