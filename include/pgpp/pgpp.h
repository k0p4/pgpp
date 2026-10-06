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
#include <cstdio>         // std::snprintf (savepoint names)
#include <cstring>        // std::strcmp (command tags)
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
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
};

struct PgppRequest;

class PgppPool final
{
public:
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

    // Synchronous (blocking)
    template<typename... Ts>
    bool execSync(const std::string& statement, const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    std::pair<bool, std::vector<RowTuple>> querySync(const std::string& statement, const Ts&... args) noexcept;

    // Future-based. A future that could not even be allocated is returned
    // invalid (valid() == false); the sync wrappers treat that as failure.
    template<typename... Ts>
    std::future<std::optional<bool>> execAsync(const std::string& statement, const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    std::future<std::pair<std::optional<bool>, std::vector<RowTuple>>>
        queryAsync(const std::string& statement, const Ts&... args) noexcept;

    // Callback-based (runs on worker thread). Fires exactly once; with nullopt
    // on the calling thread if the request could not be built or queued.
    template<typename... Ts>
    void exec(const std::string& statement,
              std::function<void(std::optional<bool>)> onDone,
              const Ts&... args) noexcept;

    template<typename RowTuple, typename... Ts>
    void query(const std::string& statement,
               std::function<void(std::optional<bool>, std::vector<RowTuple>)> onDone,
               const Ts&... args) noexcept;

    // Raw SQL (non-prepared)
    bool execRawSync(const std::string& sql) noexcept;
    std::future<std::optional<bool>> execRawAsync(const std::string& sql) noexcept;

    // Transaction (auto-rollback on exception)
    template<typename F>
    std::future<std::optional<bool>> transaction(F&& work) noexcept;

    // Public for coroutine awaitables (see pgpp_coroutines.h). Always queues.
    bool enqueueRaw(std::unique_ptr<PgppRequest> request) noexcept;

    size_t totalConnections() const noexcept;
    size_t freeConnections()  const noexcept;
    size_t busyConnections()  const noexcept;
    size_t queuedRequests()   const noexcept;

private:
    // Per-worker-thread state (defined in pgpp.cpp); each worker publishes its own
    // through t_worker for the thread's lifetime, null on every other thread.
    struct WorkerContext;
    static thread_local WorkerContext* t_worker;

    size_t      m_poolSize { std::thread::hardware_concurrency() };
    std::string m_connectionString;

    std::vector<std::unique_ptr<PgppConnection>> m_connections;
    std::atomic<size_t>                          m_connectionCount { 0 };   // for the stats getters
    std::queue<std::unique_ptr<PgppRequest>>     m_requestQueue;

    mutable std::mutex      m_queueMutex;
    std::condition_variable m_requestQueued;

    std::vector<std::thread>  m_workerThreads;
    std::atomic<bool>         m_shuttingDown { false };
    std::atomic<bool>         m_initialized  { false };
    std::atomic<size_t>       m_busyWorkers  { 0 };

    // initialize(), shutdown() and the deferred teardown run by a worker that called
    // shutdown() on itself are serialised here (REQ-PGPP-070). Workers never take this
    // mutex from inside a task, so a shutdown that is joining them cannot deadlock.
    mutable std::mutex      m_lifecycleMutex;
    std::condition_variable m_lifecycleChanged;
    bool                    m_deferredShutdown { false };   // guarded by m_lifecycleMutex
    std::thread             m_selfShutdownThread;           // joined by the next non-worker shutdown()

    mutable std::mutex     m_stmtMutex;
    std::vector<Statement> m_preparedStatements;
    std::atomic<uint32_t>  m_stmtVersion { 0 };

    bool createConnections(const PgppConnectionInfo& dbInfo) noexcept;
    bool startWorkerThreads() noexcept;
    void signalShutdown() noexcept;
    void joinWorkers(std::thread::id self) noexcept;
    void joinSelfShutdownThread() noexcept;
    void drainQueue() noexcept;
    void teardown(std::thread::id self) noexcept;
    bool initializeLocked(const PgppConnectionInfo& dbInfo, size_t poolSize) noexcept;
    void shutdownLocked() noexcept;
    void requestShutdownFromWorker() noexcept;
    void finishShutdownOnWorker() noexcept;
    void workerLoop(size_t connIdx, uint32_t stmtVersion) noexcept;
    void serveRequest(WorkerContext& ctx, PgppRequest& request) noexcept;
    std::string buildConnectionString(const PgppConnectionInfo& dbInfo) const noexcept;
    void prepareStatementsOnConnection(PgppConnection* conn) noexcept;

    // Where a request runs: inline on the calling worker's own connection when the
    // caller is one of this pool's workers (REQ-PGPP-067), queued otherwise.
    bool onOwnWorker() const noexcept;
    bool submit(std::unique_ptr<PgppRequest> request) noexcept;
    static int inlineDepth() noexcept;   // 0 off a worker; nesting level of inline requests on one

#ifdef PGPP_TESTING
    friend class PgppPoolTest;
#endif
};

struct PgppRequest {
    std::function<void(PgppConnection*)> task;
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

// snprintf into a fixed buffer; false if the text did not fit or formatting failed.
template<typename... Args>
bool formatSql(char* buffer, size_t size, const char* format, Args... args) noexcept
{
    const int written = std::snprintf(buffer, size, format, args...);
    return written > 0 && static_cast<size_t>(written) < size;
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

template<typename... Ts>
bool PgppPool::execSync(const std::string& statement, const Ts&... args) noexcept
{
    auto future = execAsync(statement, args...);
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

template<typename RowTuple, typename... Ts>
std::pair<bool, std::vector<RowTuple>> PgppPool::querySync(const std::string& statement, const Ts&... args) noexcept
{
    auto future = queryAsync<RowTuple>(statement, args...);
    if (!future.valid()) {
        return { false, {} };
    }

    try {
        auto [ok, rows] = future.get();
        return { ok.has_value() && ok.value(), std::move(rows) };
    } catch (...) {
        return { false, {} };
    }
}

template<typename... Ts>
std::future<std::optional<bool>> PgppPool::execAsync(const std::string& statement, const Ts&... args) noexcept
{
    using Result = std::optional<bool>;
    if (m_shuttingDown.load()) [[unlikely]] {
        return Internal::resolvedFuture<Result>(std::nullopt);
    }

    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        auto bound   = Internal::bindArgs(statement, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, bound](PgppConnection* conn) {
            if (!conn) {
                promise->set_value(std::nullopt);
            } else {
                promise->set_value(std::apply([conn](const std::string& stmt, const Ts&... a) {
                    return conn->execPrepared(stmt, a...);
                }, *bound));
            }
        };
        if (!submit(std::move(request))) [[unlikely]] {
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
    if (m_shuttingDown.load()) [[unlikely]] {
        return Internal::resolvedFuture<Result>({ std::nullopt, {} });
    }

    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        auto bound   = Internal::bindArgs(statement, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, bound](PgppConnection* conn) {
            if (!conn) {
                promise->set_value({ std::nullopt, {} });
            } else {
                std::vector<RowTuple> rows;
                const bool ok = std::apply([conn, &rows](const std::string& stmt, const Ts&... a) {
                    return conn->execPrepared(stmt, rows, a...);
                }, *bound);
                promise->set_value({ ok, std::move(rows) });
            }
        };
        if (!submit(std::move(request))) [[unlikely]] {
            promise->set_value({ std::nullopt, {} });
        }
        return future;
    } catch (...) {
        return Internal::resolvedFuture<Result>({ std::nullopt, {} });
    }
}

template<typename... Ts>
void PgppPool::exec(const std::string& statement,
                    std::function<void(std::optional<bool>)> onDone,
                    const Ts&... args) noexcept
{
    if (m_shuttingDown.load()) [[unlikely]] {
        Internal::invokeCallback(onDone, std::nullopt);
        return;
    }

    try {
        // onDone is copied here too (an ordinary call), so the caller's copy stays
        // valid for the fallback below.
        auto bound   = Internal::bindArgs(statement, onDone, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [bound](PgppConnection* conn) {
            std::apply([conn](const std::string& stmt, auto& done, const Ts&... a) {
                if (!conn) {
                    done(std::nullopt);
                } else {
                    done(conn->execPrepared(stmt, a...));
                }
            }, *bound);
        };
        if (!submit(std::move(request))) [[unlikely]] {
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
    if (m_shuttingDown.load()) [[unlikely]] {
        Internal::invokeCallback(onDone, std::nullopt, std::vector<RowTuple> {});
        return;
    }

    try {
        auto bound   = Internal::bindArgs(statement, onDone, args...);
        auto request = std::make_unique<PgppRequest>();
        request->task = [bound](PgppConnection* conn) {
            std::apply([conn](const std::string& stmt, auto& done, const Ts&... a) {
                if (!conn) {
                    done(std::nullopt, std::vector<RowTuple> {});
                } else {
                    std::vector<RowTuple> rows;
                    const bool ok = conn->execPrepared(stmt, rows, a...);
                    done(ok, std::move(rows));
                }
            }, *bound);
        };
        if (!submit(std::move(request))) [[unlikely]] {
            Internal::invokeCallback(onDone, std::nullopt, std::vector<RowTuple> {});
        }
    } catch (...) {
        Internal::invokeCallback(onDone, std::nullopt, std::vector<RowTuple> {});
    }
}

template<typename F>
std::future<std::optional<bool>> PgppPool::transaction(F&& work) noexcept
{
    using Result = std::optional<bool>;
    if (m_shuttingDown.load()) [[unlikely]] {
        return Internal::resolvedFuture<Result>(std::nullopt);
    }

    try {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        // The work callable is moved/copied once, here, by an ordinary call.
        auto work_   = std::make_shared<std::decay_t<F>>(std::forward<F>(work));
        auto request = std::make_unique<PgppRequest>();
        request->task = [promise, work_](PgppConnection* conn) {
            if (!conn) {
                promise->set_value(std::nullopt);
                return;
            }

            PGconn* pg = conn->connection();

            // Issued from inside transaction() work (inline on the same connection,
            // REQ-PGPP-067) the connection is already in a transaction: this one
            // becomes a savepoint named by its nesting level (REQ-PGPP-068). A
            // queued request always finds the connection idle.
            const int  depth  = PgppPool::inlineDepth();
            const bool nested = depth > 0 && PQtransactionStatus(pg) == PQTRANS_INTRANS;
            char beginSql[40];
            char commitSql[48];
            char rollbackSql[88];
            if (nested) {
                const bool formatted =
                       Internal::formatSql(beginSql,    sizeof beginSql,    "SAVEPOINT pgpp_sp_%d", depth)
                    && Internal::formatSql(commitSql,   sizeof commitSql,   "RELEASE SAVEPOINT pgpp_sp_%d", depth)
                    && Internal::formatSql(rollbackSql, sizeof rollbackSql, "ROLLBACK TO SAVEPOINT pgpp_sp_%d; RELEASE SAVEPOINT pgpp_sp_%d", depth, depth);
                if (!formatted) {
                    promise->set_value(false);
                    return;
                }
            }

            if (!Internal::execCommand(pg, nested ? beginSql : "BEGIN", nullptr)) {
                promise->set_value(false);
                return;
            }

            // work may return void, or a value convertible to bool where false
            // asks for a rollback (REQ-PGPP-066). It is user code, so it is the
            // one place a throw can originate; it is caught here (REQ-PGPP-035).
            bool workOk = true;
            try {
                if constexpr (std::is_void_v<std::invoke_result_t<std::decay_t<F>&, PgppConnection&>>) {
                    (*work_)(*conn);
                } else {
                    workOk = static_cast<bool>((*work_)(*conn));
                }
            } catch (...) {
                workOk = false;
            }

            // A failed statement leaves the transaction aborted (PQTRANS_INERROR),
            // and COMMIT on an aborted transaction answers "ROLLBACK" with
            // PGRES_COMMAND_OK. Only a healthy open transaction may be committed
            // (REQ-PGPP-065). For a savepoint, ROLLBACK TO restores the enclosing
            // transaction to a healthy state.
            if (!workOk || PQtransactionStatus(pg) != PQTRANS_INTRANS) {
                Internal::execCommand(pg, nested ? rollbackSql : "ROLLBACK", nullptr);
                promise->set_value(false);
                return;
            }

            // REQ-PGPP-036: success only with the matching command tag.
            promise->set_value(Internal::execCommand(pg, nested ? commitSql : "COMMIT",
                                                     nested ? "RELEASE" : "COMMIT"));
        };
        if (!submit(std::move(request))) [[unlikely]] {
            promise->set_value(std::nullopt);
        }
        return future;
    } catch (...) {
        return Internal::resolvedFuture<Result>(std::nullopt);
    }
}
