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
#include <chrono>
#include <condition_variable>
#include <cstring>        // std::strcmp (command tags)
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>          // std::tuple, std::apply (bound request arguments)
#include <type_traits>    // std::decay_t, std::invoke_result_t, std::is_void_v
#include <utility>        // std::move, std::forward, std::pair
#include <vector>

struct PgppConnectionInfo {
    std::string dbname;
    std::string host;
    std::string sslmode;
    std::string options;
    std::string user;
    std::string password;
    uint16_t    port { 5432 };

    // How long acquire() and the synchronous API wait for a free connection
    // before giving up (REQ-PGPP-067). 0 waits without limit.
    std::chrono::milliseconds acquireTimeout { 30000 };
};

struct PgppRequest;

// A pool of connections handed out as leases, plus an executor for the
// asynchronous APIs.
//
//   - The sync API (execSync, querySync, execRawSync, transactionSync) acquires
//     a connection on the calling thread, runs, and returns it. It never uses
//     the executor and never hops threads (REQ-PGPP-068).
//   - The future, callback, coroutine and transaction() APIs queue a task on the
//     executor; the task acquires a connection, runs, returns it, then completes
//     the future / fires the callback / resumes the coroutine (REQ-PGPP-069).
//   - Inside transaction() work, use the connection you were given: a pool call
//     from there acquires a second connection (REQ-PGPP-067).
//   - Do not block on a future from inside a callback (that is an executor
//     thread waiting on the same executor); the sync API is fine there.
class PgppPool final
{
    struct Slot;   // one pooled connection (pgpp.cpp)

public:
    // Exclusive use of one connection until destroyed or released.
    class Lease
    {
    public:
        Lease() noexcept = default;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        explicit operator bool() const noexcept { return m_slot != nullptr; }
        PgppConnection* get() const noexcept;
        PgppConnection* operator->() const noexcept { return get(); }
        PgppConnection& operator*() const noexcept { return *get(); }

        void release() noexcept;   // return the connection now

    private:
        friend class PgppPool;
        Lease(PgppPool* pool, Slot* slot) noexcept : m_pool(pool), m_slot(slot) {}

        PgppPool* m_pool { nullptr };
        Slot*     m_slot { nullptr };
    };

    PgppPool();
    ~PgppPool();

    PgppPool(const PgppPool&) = delete;
    PgppPool& operator=(const PgppPool&) = delete;

    bool initialize(const PgppConnectionInfo& dbInfo, size_t poolSize = 0) noexcept;
    void shutdown() noexcept;
    bool isInitialized() const noexcept;
    void prepareStatement(const Statement& statement) noexcept;

    // Every public function is noexcept (REQ-PGPP-061): whatever fails inside
    // (allocation, copying an argument, a mutex or thread error, a throwing
    // user callback) is reported as the function's failure value instead.

    // A connection for the calling thread; empty if none became free within
    // acquireTimeout, or the pool is not running.
    Lease acquire() noexcept;

    // Synchronous (on the calling thread, no executor involved)
    template<typename... Ts>
    bool execSync(const std::string& statement, const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    std::pair<bool, std::vector<RowTuple>> querySync(const std::string& statement, const Ts&... args) noexcept;

    bool execRawSync(const std::string& sql) noexcept;

    template<typename F>
    bool transactionSync(F&& work) noexcept;

    // Future-based. A future that could not even be allocated is returned
    // invalid (valid() == false).
    template<typename... Ts>
    std::future<std::optional<bool>> execAsync(const std::string& statement, const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    std::future<std::pair<std::optional<bool>, std::vector<RowTuple>>>
        queryAsync(const std::string& statement, const Ts&... args) noexcept;

    std::future<std::optional<bool>> execRawAsync(const std::string& sql) noexcept;

    template<typename F>
    std::future<std::optional<bool>> transaction(F&& work) noexcept;

    // Callback-based (runs on an executor thread, after the statement's
    // connection was returned). Fires exactly once; with nullopt on the calling
    // thread if the request could not be built or queued.
    template<typename... Ts>
    void exec(const std::string& statement,
              std::function<void(std::optional<bool>)> onDone,
              const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    void query(const std::string& statement,
               std::function<void(std::optional<bool>, std::vector<RowTuple>)> onDone,
               const Ts&... args) noexcept;

    // Queues a task on the executor. Public for coroutine awaitables (see
    // pgpp_coroutines.h). false if the pool is not running.
    bool enqueueRaw(std::unique_ptr<PgppRequest> request) noexcept;

    size_t totalConnections() const noexcept;
    size_t freeConnections()  const noexcept;
    size_t busyConnections()  const noexcept;
    size_t queuedRequests()   const noexcept;

private:
    // ── Connections ──────────────────────────────────────────────────────────
    size_t      m_poolSize { std::thread::hardware_concurrency() };
    std::string m_connectionString;

    mutable std::mutex        m_mutex;            // m_all, m_idle, m_leased, m_stopping, m_acquireTimeout
    std::condition_variable   m_idleAvailable;
    std::vector<std::unique_ptr<Slot>> m_all;     // fixed between initialize and shutdown
    std::vector<Slot*>        m_idle;             // free-list (LIFO)
    size_t                    m_leased { 0 };
    bool                      m_stopping { false };
    std::chrono::milliseconds m_acquireTimeout { 30000 };

    // ── Executor ─────────────────────────────────────────────────────────────
    mutable std::mutex                       m_taskMutex;   // m_tasks, m_taskStopping, m_threadIds
    std::condition_variable                  m_taskAvailable;
    std::deque<std::unique_ptr<PgppRequest>> m_tasks;
    bool                                     m_taskStopping { false };
    std::vector<std::thread>                 m_threads;     // touched under m_lifecycleMutex only
    std::vector<std::thread::id>             m_threadIds;

    // ── Lifecycle ────────────────────────────────────────────────────────────
    std::mutex        m_lifecycleMutex;   // initialize / shutdown from non-executor threads
    std::atomic<bool> m_initialized { false };

    // ── Statements ───────────────────────────────────────────────────────────
    mutable std::mutex     m_stmtMutex;
    std::vector<Statement> m_preparedStatements;

    Lease acquireFor(std::optional<std::chrono::milliseconds> timeout) noexcept;
    bool  prepareForUse(Slot& slot) noexcept;
    void  releaseSlot(Slot* slot) noexcept;
    bool  createConnections(const PgppConnectionInfo& dbInfo) noexcept;
    void  closeConnections() noexcept;
    void  waitForLeases() noexcept;

    bool  startExecutor() noexcept;
    void  executorLoop() noexcept;
    void  runTask(PgppRequest& request, Lease lease) noexcept;
    void  stopExecutor() noexcept;
    void  joinExecutor() noexcept;
    bool  onExecutorThread() const noexcept;

    bool  initializeLocked(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept;
    void  shutdownLocked() noexcept;
    void  requestStop() noexcept;

    std::string buildConnectionString(const PgppConnectionInfo& dbInfo) const noexcept;

#ifdef PGPP_TESTING
    friend class PgppPoolTest;
#endif
};

// A unit of work for the executor: runs with a lease (empty when the pool is
// shutting down and the request is drained) and completes its own future,
// callback or coroutine.
struct PgppRequest {
    std::function<void(PgppPool::Lease)> task;
};

// ── Template implementations ─────────────────────────────────────────────────

namespace Internal {

// A future that already holds `value`. If even that cannot be allocated, an
// invalid future (valid() == false) — the sync wrappers treat it as failure.
template<typename T>
std::future<T> resolvedFuture(T value) noexcept
{
    try {
        std::promise<T> promise;
        promise.set_value(std::move(value));
        return promise.get_future();
    } catch (...) {
        return {};
    }
}

// Runs a user callback on the calling thread; whatever it throws stays here.
template<typename Callback, typename... Args>
void invokeCallback(Callback& callback, Args&&... args) noexcept
{
    try {
        callback(std::forward<Args>(args)...);
    } catch (...) {
    }
}

// One command, success = PGRES_COMMAND_OK and, when given, the expected command
// tag. PostgreSQL answers COMMIT on an aborted transaction with "ROLLBACK" and
// PGRES_COMMAND_OK, so the tag is the only honest signal (REQ-PGPP-036).
inline bool execCommand(PGconn* pg, const char* sql, const char* expectedTag) noexcept
{
    PGresult* res = PQexec(pg, sql);
    const bool ok = res != nullptr
        && PQresultStatus(res) == PGRES_COMMAND_OK
        && (expectedTag == nullptr || std::strcmp(PQcmdStatus(res), expectedTag) == 0);
    PQclear(res);
    return ok;
}

// BEGIN, work, COMMIT on one connection. false if BEGIN failed, work threw or
// returned false (REQ-PGPP-035/066), the connection is not in a healthy open
// transaction afterwards (REQ-PGPP-065), or COMMIT did not answer "COMMIT".
template<typename Work>
bool runTransaction(PgppConnection& conn, Work& work) noexcept
{
    PGconn* pg = conn.connection();
    if (!execCommand(pg, "BEGIN", nullptr)) {
        return false;
    }

    // work may return void, or a value convertible to bool where false asks
    // for a rollback. It is user code, so it is the one place a throw can
    // originate; it is caught here.
    bool workOk = true;
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Work&, PgppConnection&>>) {
            work(conn);
        } else {
            workOk = static_cast<bool>(work(conn));
        }
    } catch (...) {
        workOk = false;
    }

    // A failed statement leaves the transaction aborted (PQTRANS_INERROR), and
    // COMMIT on an aborted transaction answers "ROLLBACK" with PGRES_COMMAND_OK.
    if (!workOk || PQtransactionStatus(pg) != PQTRANS_INTRANS) {
        execCommand(pg, "ROLLBACK", nullptr);
        return false;
    }
    return execCommand(pg, "COMMIT", "COMMIT");
}

// The caller's statement name and arguments, copied once into shared storage
// by an ordinary call (where a throwing copy is an ordinary exception). The
// request task then captures only this pointer, so building it cannot throw,
// and the arguments are not copied a second time into the std::function.
template<typename... Ts>
using BoundArgs = std::shared_ptr<std::tuple<std::string, Ts...>>;

template<typename... Ts>
BoundArgs<Ts...> bindArgs(const std::string& statement, const Ts&... args)
{
    return std::make_shared<std::tuple<std::string, Ts...>>(statement, args...);
}

} // namespace Internal

// ── Synchronous ──────────────────────────────────────────────────────────────

template<typename... Ts>
bool PgppPool::execSync(const std::string& statement, const Ts&... args) noexcept
{
    Lease lease = acquire();
    if (!lease) {
        return false;
    }
    return lease->execPrepared(statement, args...);
}

template<typename RowTuple, typename... Ts>
std::pair<bool, std::vector<RowTuple>> PgppPool::querySync(const std::string& statement, const Ts&... args) noexcept
{
    std::vector<RowTuple> rows;
    Lease lease = acquire();
    if (!lease) {
        return { false, std::move(rows) };
    }
    const bool ok = lease->execPrepared(statement, rows, args...);
    return { ok, std::move(rows) };
}

template<typename F>
bool PgppPool::transactionSync(F&& work) noexcept
{
    Lease lease = acquire();
    if (!lease) {
        return false;
    }
    return Internal::runTransaction(*lease, work);
}

// ── Future-based ─────────────────────────────────────────────────────────────

template<typename... Ts>
std::future<std::optional<bool>> PgppPool::execAsync(const std::string& statement, const Ts&... args) noexcept
{
    using Result = std::optional<bool>;
    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        auto bound   = Internal::bindArgs(statement, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, bound](Lease lease) {
            if (!lease) {
                promise->set_value(std::nullopt);
                return;
            }
            const bool ok = std::apply([&lease](const std::string& stmt, const Ts&... a) {
                return lease->execPrepared(stmt, a...);
            }, *bound);
            lease.release();
            promise->set_value(ok);
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            promise->set_value(std::nullopt);
        }
        return future;
    } catch (...) {
        // Building or queueing the request failed (allocation, copying an argument, ...).
        return Internal::resolvedFuture<Result>(std::nullopt);
    }
}

template<typename RowTuple, typename... Ts>
std::future<std::pair<std::optional<bool>, std::vector<RowTuple>>>
PgppPool::queryAsync(const std::string& statement, const Ts&... args) noexcept
{
    using Result = std::pair<std::optional<bool>, std::vector<RowTuple>>;
    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        auto bound   = Internal::bindArgs(statement, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, bound](Lease lease) {
            if (!lease) {
                promise->set_value({ std::nullopt, {} });
                return;
            }
            std::vector<RowTuple> rows;
            const bool ok = std::apply([&lease, &rows](const std::string& stmt, const Ts&... a) {
                return lease->execPrepared(stmt, rows, a...);
            }, *bound);
            lease.release();
            promise->set_value({ ok, std::move(rows) });
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            promise->set_value({ std::nullopt, {} });
        }
        return future;
    } catch (...) {
        return Internal::resolvedFuture<Result>({ std::nullopt, {} });
    }
}

template<typename F>
std::future<std::optional<bool>> PgppPool::transaction(F&& work) noexcept
{
    using Result = std::optional<bool>;
    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        // The work callable is moved/copied once, here, by an ordinary call.
        auto work_   = std::make_shared<std::decay_t<F>>(std::forward<F>(work));
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, work_](Lease lease) {
            if (!lease) {
                promise->set_value(std::nullopt);
                return;
            }
            const bool ok = Internal::runTransaction(*lease, *work_);
            lease.release();
            promise->set_value(ok);
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            promise->set_value(std::nullopt);
        }
        return future;
    } catch (...) {
        return Internal::resolvedFuture<Result>(std::nullopt);
    }
}

// ── Callback-based ───────────────────────────────────────────────────────────

template<typename... Ts>
void PgppPool::exec(const std::string& statement,
                    std::function<void(std::optional<bool>)> onDone,
                    const Ts&... args) noexcept
{
    try {
        // onDone is copied here too (an ordinary call), so the caller's copy stays
        // valid for the fallback below.
        auto bound   = Internal::bindArgs(statement, onDone, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [bound](Lease lease) {
            std::apply([&lease](const std::string& stmt, auto& done, const Ts&... a) {
                if (!lease) {
                    done(std::nullopt);
                } else {
                    const bool ok = lease->execPrepared(stmt, a...);
                    lease.release();   // the callback runs with the connection back in the pool
                    done(ok);
                }
            }, *bound);
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            Internal::invokeCallback(onDone, std::nullopt);
        }
    } catch (...) {
        Internal::invokeCallback(onDone, std::nullopt);
    }
}

template<typename RowTuple, typename... Ts>
void PgppPool::query(const std::string& statement,
                     std::function<void(std::optional<bool>, std::vector<RowTuple>)> onDone,
                     const Ts&... args) noexcept
{
    try {
        auto bound   = Internal::bindArgs(statement, onDone, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [bound](Lease lease) {
            std::apply([&lease](const std::string& stmt, auto& done, const Ts&... a) {
                if (!lease) {
                    done(std::nullopt, std::vector<RowTuple> {});
                } else {
                    std::vector<RowTuple> rows;
                    const bool ok = lease->execPrepared(stmt, rows, a...);
                    lease.release();
                    done(ok, std::move(rows));
                }
            }, *bound);
        };
        if (!enqueueRaw(std::move(request))) [[unlikely]] {
            Internal::invokeCallback(onDone, std::nullopt, std::vector<RowTuple> {});
        }
    } catch (...) {
        Internal::invokeCallback(onDone, std::nullopt, std::vector<RowTuple> {});
    }
}
