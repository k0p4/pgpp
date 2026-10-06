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

#include <algorithm>
#include <chrono>
#include <thread>

PGPP_DEFINE_LOG_MODULE(PgppPool)

// One pooled connection and how far it has caught up with the statement list.
struct PgppPool::Slot {
    std::unique_ptr<PgppConnection> conn;
    size_t preparedCount { 0 };   // statements [0, preparedCount) are prepared on conn
};

// ── Lease ────────────────────────────────────────────────────────────────────

PgppPool::Lease::Lease(Lease&& other) noexcept
    : m_pool(other.m_pool)
    , m_slot(other.m_slot)
{
    other.m_pool = nullptr;
    other.m_slot = nullptr;
}

PgppPool::Lease& PgppPool::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        release();
        m_pool = other.m_pool;
        m_slot = other.m_slot;
        other.m_pool = nullptr;
        other.m_slot = nullptr;
    }
    return *this;
}

PgppPool::Lease::~Lease()
{
    release();
}

PgppConnection* PgppPool::Lease::get() const noexcept
{
    return m_slot != nullptr ? m_slot->conn.get() : nullptr;
}

void PgppPool::Lease::release() noexcept
{
    if (m_slot != nullptr) {
        m_pool->releaseSlot(m_slot);
        m_slot = nullptr;
        m_pool = nullptr;
    }
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

// ── Connections ──────────────────────────────────────────────────────────────

bool PgppPool::createConnections(const PgppConnectionInfo& dbInfo) noexcept
{
    try {
        m_connectionString = buildConnectionString(dbInfo);
        if (m_connectionString.empty()) {
            PGPP_LOGE << "Failed to build connection string";
            return false;
        }

        for (size_t i = 0; i < m_poolSize; ++i) {
            auto slot = std::make_unique<Slot>();
            slot->conn = std::make_unique<PgppConnection>();
            if (!slot->conn->open(m_connectionString)) [[unlikely]] {
                PGPP_LOGE << "Failed to create connection " << i << ": " << slot->conn->lastError();
                return false;
            }
            if (!prepareForUse(*slot)) [[unlikely]] {
                return false;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_idle.push_back(slot.get());
            m_all.push_back(std::move(slot));
        }

        PGPP_LOGD << "Created " << m_poolSize << " connections";
        return true;
    } catch (...) {
        PGPP_LOGE << "Failed to create connections (allocation or mutex error)";
        return false;
    }
}

void PgppPool::closeConnections() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_idle.clear();
        m_all.clear();
        m_leased = 0;
    } catch (...) {
        PGPP_LOGE << "Failed to lock while closing connections";
        m_idle.clear();
        m_all.clear();
        m_leased = 0;
    }
}

// Reconnects a lost connection and prepares the statements registered since
// this connection last caught up (REQ-PGPP-042/043, REQ-PGPP-026).
bool PgppPool::prepareForUse(Slot& slot) noexcept
{
    PgppConnection& conn = *slot.conn;

    if (!conn.isOpen()) [[unlikely]] {
        PGPP_LOGW << "Connection lost, reconnecting...";
        conn.reset();
        if (!conn.isOpen()) {
            PGPP_LOGE << "Reconnect failed";
            return false;
        }
        slot.preparedCount = 0;   // the server session is new: nothing is prepared
        PGPP_LOGD << "Connection restored";
    }

    std::vector<Statement> pending;
    try {
        std::lock_guard<std::mutex> lock(m_stmtMutex);
        if (slot.preparedCount < m_preparedStatements.size()) {
            pending.assign(m_preparedStatements.begin() + static_cast<std::ptrdiff_t>(slot.preparedCount),
                           m_preparedStatements.end());
        }
    } catch (...) {
        // Cannot copy the new statements now; the connection is still usable and
        // catches up on a later acquire.
        PGPP_LOGE << "Failed to read the statement list (allocation or mutex error)";
        return true;
    }

    for (const auto& stmt : pending) {
        if (!conn.prepare(stmt)) [[unlikely]] {
            PGPP_LOGW << "Failed to prepare: " << stmt.statementName;
        }
    }
    slot.preparedCount += pending.size();
    return true;
}

PgppPool::Lease PgppPool::acquire() noexcept
{
    std::chrono::milliseconds timeout { 0 };
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        timeout = m_acquireTimeout;
    } catch (...) {
    }
    if (timeout.count() > 0) {
        return acquireFor(timeout);
    } else {
        return acquireFor(std::nullopt);
    }
}

PgppPool::Lease PgppPool::acquireFor(std::optional<std::chrono::milliseconds> timeout) noexcept
{
    Slot* slot = nullptr;
    try {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_all.empty() || m_stopping) {
            return {};   // not running
        }
        auto ready = [this] { return m_stopping || !m_idle.empty(); };
        if (timeout.has_value()) {
            if (!m_idleAvailable.wait_for(lock, *timeout, ready)) {
                PGPP_LOGW << "No connection became free within " << timeout->count() << " ms";
                return {};
            }
        } else {
            m_idleAvailable.wait(lock, ready);
        }
        if (m_stopping) {
            return {};
        }
        slot = m_idle.back();
        m_idle.pop_back();
        ++m_leased;
    } catch (...) {
        PGPP_LOGE << "Failed to wait for a connection (mutex or condition-variable error)";
        return {};
    }

    // On the caller's thread, outside the lock: reconnect / catch up on statements.
    if (!prepareForUse(*slot)) [[unlikely]] {
        releaseSlot(slot);
        return {};
    }
    return Lease(this, slot);
}

// A connection comes back clean: a caller that left a transaction open or
// aborted must not hand it to the next caller (REQ-PGPP-072).
void PgppPool::releaseSlot(Slot* slot) noexcept
{
    PgppConnection& conn = *slot->conn;
    if (conn.isOpen()) {
        const PGTransactionStatusType status = PQtransactionStatus(conn.connection());
        if (status == PQTRANS_INTRANS || status == PQTRANS_INERROR) {
            PGPP_LOGW << "Connection returned inside "
                      << (status == PQTRANS_INERROR ? "an aborted" : "an open")
                      << " transaction: rolling back";
            Internal::execCommand(conn.connection(), "ROLLBACK", nullptr);
        }
    }

    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_leased;
        if (m_stopping) {
            conn.close();
        } else {
            m_idle.push_back(slot);
        }
    } catch (...) {
        PGPP_LOGE << "Failed to lock while returning a connection";
    }
    m_idleAvailable.notify_all();   // a waiting acquire, or shutdown waiting for leases
}

void PgppPool::waitForLeases() noexcept
{
    try {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_leased > 0) {
            PGPP_LOGW << "shutdown: waiting for " << m_leased << " leased connection(s) to be returned";
        }
        m_idleAvailable.wait(lock, [this] { return m_leased == 0; });
    } catch (...) {
        PGPP_LOGE << "Failed to wait for leased connections";
    }
}

// ── Executor ─────────────────────────────────────────────────────────────────

bool PgppPool::startExecutor() noexcept
{
    try {
        m_threads.reserve(m_poolSize);
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_taskStopping = false;
        for (size_t i = 0; i < m_poolSize; ++i) {
            m_threads.emplace_back(&PgppPool::executorLoop, this);
            m_threadIds.push_back(m_threads.back().get_id());
        }
        PGPP_LOGD << "Started " << m_poolSize << " executor threads";
        return true;
    } catch (...) {
        // std::system_error from thread creation, or allocation: the caller stops
        // whatever did start.
        PGPP_LOGE << "Failed to start executor threads";
        return false;
    }
}

void PgppPool::executorLoop() noexcept
{
    while (true) {
        std::unique_ptr<PgppRequest> request;
        bool stopping = false;
        try {
            std::unique_lock<std::mutex> lock(m_taskMutex);
            m_taskAvailable.wait(lock, [this] { return m_taskStopping || !m_tasks.empty(); });
            if (m_tasks.empty()) {
                return;   // stopping and nothing left to drain
            }
            request  = std::move(m_tasks.front());
            m_tasks.pop_front();
            stopping = m_taskStopping;
        } catch (...) {
            // std::system_error from the mutex or condition variable: back off,
            // then try again; nothing was dequeued.
            PGPP_LOGE << "Executor: failed to wait for a task";
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } catch (...) {
            }
            continue;
        }

        // After stop() the remaining tasks are drained: they complete with no
        // connection (nullopt) instead of running (REQ-PGPP-021).
        Lease lease;
        if (!stopping) {
            lease = acquireFor(std::nullopt);
        }
        runTask(*request, std::move(lease));
    }
}

void PgppPool::runTask(PgppRequest& request, Lease lease) noexcept
{
    try {
        request.task(std::move(lease));
    } catch (const std::exception& e) {
        PGPP_LOGE << "Request exception: " << e.what();
    } catch (...) {
        PGPP_LOGE << "Unknown request exception";
    }
}

bool PgppPool::enqueueRaw(std::unique_ptr<PgppRequest> request) noexcept
{
    if (!m_initialized.load()) [[unlikely]] {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        if (m_taskStopping) [[unlikely]] {
            return false;
        }
        m_tasks.push_back(std::move(request));
    } catch (...) {
        PGPP_LOGE << "Failed to enqueue a request (allocation or mutex error)";
        return false;
    }
    m_taskAvailable.notify_one();
    return true;
}

void PgppPool::stopExecutor() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_taskStopping = true;
    } catch (...) {
        PGPP_LOGE << "Failed to lock while stopping the executor";
        m_taskStopping = true;
    }
    m_taskAvailable.notify_all();
}

void PgppPool::joinExecutor() noexcept
{
    for (auto& thread : m_threads) {
        try {
            if (thread.joinable()) {
                thread.join();
            }
        } catch (...) {
            // Cannot happen for another thread that is joinable; a thread left
            // joinable would terminate the process, so detach is the lesser evil.
            PGPP_LOGE << "Failed to join an executor thread";
            if (thread.joinable()) {
                thread.detach();
            }
        }
    }
    m_threads.clear();
    try {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_threadIds.clear();
    } catch (...) {
        m_threadIds.clear();
    }
}

bool PgppPool::onExecutorThread() const noexcept
{
    const auto self = std::this_thread::get_id();
    try {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        return std::find(m_threadIds.begin(), m_threadIds.end(), self) != m_threadIds.end();
    } catch (...) {
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

bool PgppPool::initialize(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept
{
    if (onExecutorThread()) {
        // The executor exists, so the pool is running; the lifecycle lock may be
        // held by a shutdown() that is joining this very thread.
        PGPP_LOGW << "initialize() called from an executor thread";
        return m_initialized.load();
    }

    try {
        std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
        return initializeLocked(dbInfo, poolSize);
    } catch (...) {
        PGPP_LOGE << "initialize: lifecycle lock failed";
        return initializeLocked(dbInfo, poolSize);
    }
}

bool PgppPool::initializeLocked(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept
{
    if (m_initialized.load()) [[unlikely]] {
        PGPP_LOGW << "Already initialized";
        return true;
    }

    // A shutdown() issued from an executor thread leaves the threads to exit on
    // their own; finish that teardown first (REQ-PGPP-070).
    shutdownLocked();

    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping       = false;
        m_acquireTimeout = dbInfo.acquireTimeout;
    } catch (...) {
        PGPP_LOGE << "initialize: failed to lock";
        return false;
    }

    if (poolSize > 0) [[likely]] {
        m_poolSize = poolSize;
    }

    if (!createConnections(dbInfo)) [[unlikely]] {
        closeConnections();
        return false;
    }
    if (!startExecutor()) [[unlikely]] {
        stopExecutor();
        joinExecutor();
        closeConnections();
        return false;
    }

    m_initialized.store(true);
    PGPP_LOGD << "Pool initialized with " << m_poolSize << " connections";
    return true;
}

// Stop accepting work and wake everyone: acquire() returns empty, the executor
// drains its queue with empty leases and its threads exit.
void PgppPool::requestStop() noexcept
{
    m_initialized.store(false);
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    } catch (...) {
        m_stopping = true;
    }
    m_idleAvailable.notify_all();
    stopExecutor();
}

void PgppPool::shutdown() noexcept
{
    if (onExecutorThread()) {
        // A thread cannot join itself: stop and return (REQ-PGPP-070). The threads
        // exit on their own; the destructor or the next initialize() joins them.
        PGPP_LOGW << "shutdown() called from an executor thread: returning without waiting";
        requestStop();
        return;
    }

    try {
        std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
        shutdownLocked();
    } catch (...) {
        PGPP_LOGE << "shutdown: lifecycle lock failed";
        shutdownLocked();
    }
}

void PgppPool::shutdownLocked() noexcept
{
    bool haveConnections = false;
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        haveConnections = !m_all.empty();
    } catch (...) {
        haveConnections = true;
    }
    if (!m_initialized.load() && m_threads.empty() && !haveConnections) {
        return;   // nothing to do (REQ-PGPP-020)
    }

    requestStop();
    joinExecutor();    // the threads drain the queue (nullopt) and exit
    waitForLeases();   // leases held by callers of acquire()
    closeConnections();
    PGPP_LOGD << "Pool shut down";
}

bool PgppPool::isInitialized() const noexcept { return m_initialized.load(); }

// ── Statements ───────────────────────────────────────────────────────────────

void PgppPool::prepareStatement(const Statement& statement) noexcept
{
    // Connections catch up on the next acquire (REQ-PGPP-026): nothing to wake.
    try {
        std::lock_guard<std::mutex> lock(m_stmtMutex);
        m_preparedStatements.push_back(statement);
    } catch (...) {
        PGPP_LOGE << "Failed to register statement " << statement.statementName << " (allocation or mutex error)";
    }
}

// ── Raw SQL ──────────────────────────────────────────────────────────────────

bool PgppPool::execRawSync(const std::string& sql) noexcept
{
    Lease lease = acquire();
    if (!lease) {
        return false;
    }
    return lease->execRaw(sql);
}

std::future<std::optional<bool>> PgppPool::execRawAsync(const std::string& sql) noexcept
{
    using Result = std::optional<bool>;
    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        auto request = std::make_unique<PgppRequest>();
        request->task = [sql, promise](Lease lease) {
            if (!lease) {
                promise->set_value(std::nullopt);
                return;
            }
            const bool ok = lease->execRaw(sql);
            lease.release();
            promise->set_value(ok);
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

// ── Statistics ───────────────────────────────────────────────────────────────

size_t PgppPool::totalConnections() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_all.size();
    } catch (...) {
        return 0;
    }
}

size_t PgppPool::freeConnections() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_idle.size();
    } catch (...) {
        return 0;
    }
}

size_t PgppPool::busyConnections() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_leased;
    } catch (...) {
        return 0;
    }
}

size_t PgppPool::queuedRequests() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        return m_tasks.size();
    } catch (...) {
        return 0;
    }
}
