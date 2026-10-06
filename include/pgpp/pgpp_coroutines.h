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

#include <pgpp/pgpp.h>

#include <coroutine>
#include <exception>      // std::current_exception, std::rethrow_exception
#include <iostream>       // std::cerr in FireAndForget::unhandled_exception
#include <memory>         // std::make_unique
#include <optional>
#include <string>
#include <tuple>          // std::tuple, std::apply
#include <type_traits>    // std::decay_t
#include <utility>        // std::move, std::forward, std::pair
#include <vector>

// Minimal coroutine return type for fire-and-forget async tasks.
// Starts immediately, self-destructs on completion.
struct FireAndForget
{
    struct promise_type
    {
        FireAndForget get_return_object() noexcept { return {}; }
        // Makes the compiler allocate the frame with std::nothrow: if that fails
        // the coroutine body never runs and the call returns this instead of
        // throwing std::bad_alloc (REQ-PGPP-061).
        static FireAndForget get_return_object_on_allocation_failure() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend()   noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept
        {
            try { std::rethrow_exception(std::current_exception()); }
            catch (const std::exception& e) {
                std::cerr << "[FireAndForget] unhandled exception: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "[FireAndForget] unhandled unknown exception" << std::endl;
            }
        }
    };
};

// Lifetime contract for the awaitables below.
//
// The awaitable lives on the awaiting coroutine's frame and keeps its state
// inline. `await_suspend` hands the pool a PgppRequest whose task captures
// `this` and runs on a worker thread (or during `shutdown()` drain, with
// `conn == nullptr`). The task always ends with exactly one
// `handle.resume()` (pgpp itself never throws, REQ-PGPP-061), so the frame
// is alive whenever the task touches it, provided the caller keeps the
// coroutine alive until it is resumed. FireAndForget satisfies this by
// construction (its handle isn't exposed, and it self-destructs only after
// the final return). Destroying a suspended coroutine from outside is UB.
//
// After `enqueueRaw` succeeds the worker may resume (and destroy) the frame
// before `await_suspend` returns, so nothing may touch `this` after it.

namespace Internal {

// Shared part of DbExecAwaitable / DbResultAwaitable. `Derived::execute`
// runs the statement on the worker's connection and returns its result.
template<typename Derived, typename... Ts>
class DbAwaitableBase
{
public:
    DbAwaitableBase(PgppPool& db, std::string statement, Ts... args) noexcept
        : m_db(db)
    {
        try {
            m_statement = std::move(statement);
            m_args.emplace(std::move(args)...);
        } catch (...) {
            // Could not store the statement or its arguments: the awaitable
            // completes at once with nullopt (see await_ready).
            m_args.reset();
        }
    }

    // A failed awaitable (see coExec / coQuery): completes immediately with nullopt.
    explicit DbAwaitableBase(PgppPool& db) noexcept
        : m_db(db)
    {}

    bool await_ready() const noexcept { return !m_args.has_value(); }

    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        try {
            auto request = std::make_unique<PgppRequest>();
            request->task = [this, handle](PgppConnection* conn) {
                if (conn) {
                    m_result = std::apply(
                        [&](const auto&... a) -> bool {
                            return static_cast<Derived*>(this)->execute(*conn, a...);
                        }, *m_args);
                }
                handle.resume();
            };
            return m_db.enqueueRaw(std::move(request));
        } catch (...) {
            // Could not build the request: resume at once, result stays nullopt.
            return false;
        }
    }

protected:
    PgppPool&                          m_db;
    std::string                        m_statement;
    std::optional<std::tuple<Ts...>>   m_args;
    std::optional<bool>                m_result;
};

} // namespace Internal

// co_await coExec(db, "stmt", args...) -> std::optional<bool>
template<typename... Ts>
class DbExecAwaitable : public Internal::DbAwaitableBase<DbExecAwaitable<Ts...>, Ts...>
{
    using Base = Internal::DbAwaitableBase<DbExecAwaitable<Ts...>, Ts...>;
    friend Base;

public:
    using Base::Base;

    std::optional<bool> await_resume() noexcept { return this->m_result; }

private:
    bool execute(PgppConnection& conn, const Ts&... args) noexcept
    {
        return conn.execPrepared(this->m_statement, args...);
    }
};

// co_await coQuery<RowTuple>(db, "stmt", args...)
//   -> std::pair<std::optional<bool>, std::vector<RowTuple>>
template<typename RowTuple, typename... Ts>
class DbResultAwaitable : public Internal::DbAwaitableBase<DbResultAwaitable<RowTuple, Ts...>, Ts...>
{
    using Base = Internal::DbAwaitableBase<DbResultAwaitable<RowTuple, Ts...>, Ts...>;
    friend Base;

public:
    using Rows = std::vector<RowTuple>;
    using Base::Base;

    std::pair<std::optional<bool>, Rows> await_resume() noexcept
    {
        return { this->m_result, std::move(m_rows) };
    }

private:
    bool execute(PgppConnection& conn, const Ts&... args) noexcept
    {
        return conn.execPrepared(this->m_statement, m_rows, args...);
    }

    Rows m_rows;
};

// Factory helpers. Copying an argument into the awaitable can throw; the
// factories catch that and return an awaitable that completes with nullopt,
// so co_await never throws out of pgpp (REQ-PGPP-061).
template<typename... Ts>
auto coExec(PgppPool& db, std::string statement, Ts&&... args) noexcept
{
    using Awaitable = DbExecAwaitable<std::decay_t<Ts>...>;
    try {
        return Awaitable(db, std::move(statement), std::forward<Ts>(args)...);
    } catch (...) {
        return Awaitable(db);
    }
}

template<typename RowTuple, typename... Ts>
auto coQuery(PgppPool& db, std::string statement, Ts&&... args) noexcept
{
    using Awaitable = DbResultAwaitable<RowTuple, std::decay_t<Ts>...>;
    try {
        return Awaitable(db, std::move(statement), std::forward<Ts>(args)...);
    } catch (...) {
        return Awaitable(db);
    }
}

// Backward compatibility
template<typename... Ts>
auto coExecPrepared(PgppPool& db, std::string statement, Ts&&... args) noexcept
{
    return coExec(db, std::move(statement), std::forward<Ts>(args)...);
}

template<typename RowTuple, typename... Ts>
auto coExecPreparedWithResult(PgppPool& db, std::string statement, Ts&&... args) noexcept
{
    return coQuery<RowTuple>(db, std::move(statement), std::forward<Ts>(args)...);
}
