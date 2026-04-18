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
#include <exception>
#include <iostream>
#include <optional>
#include <tuple>
#include <vector>

// Minimal coroutine return type for fire-and-forget async tasks.
// Starts immediately, self-destructs on completion.
struct FireAndForget
{
    struct promise_type
    {
        FireAndForget get_return_object() noexcept { return {}; }
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

// Shared state for coroutine awaitables.
//
// Lifetime contract: the awaitable lives on the caller's coroutine frame.
// Its `await_suspend` hands a PgppRequest to the pool, whose `task` lambda
// may run on a worker thread at an arbitrary later point (including during
// `shutdown()`, when `conn` is nullptr).
//
// Capturing `this` in that lambda would dangle the moment the coroutine
// frame is destroyed — which for FireAndForget is tied to coroutine
// completion and therefore safe in practice, but *not* guaranteed for
// caller-defined coroutine return types that expose `handle.destroy()`.
// To make the awaitable safe regardless of caller, all mutable state lives
// in a heap-allocated struct owned by a shared_ptr: the awaitable holds one
// ref (to be read by `await_resume`), and the worker's lambda captures a
// copy by value. Destroying either side is harmless — the worker writes to
// valid memory, and on resume the awaitable reads the value the worker
// already wrote.
//
// Note: `handle.resume()` on a destroyed coroutine handle is still UB. The
// library does not protect against that — callers must keep the awaiting
// coroutine alive until the awaitable yields. FireAndForget satisfies this
// by construction (its handle isn't exposed, and it self-destructs only
// after the final return).

// co_await coExec(db, "stmt", args...) -> std::optional<bool>
template<typename... Ts>
class DbExecAwaitable
{
public:
    DbExecAwaitable(PgppPool& db, std::string statement, Ts... args)
        : m_state(std::make_shared<State>(db, std::move(statement), std::move(args)...))
    {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto state = m_state;                // captured by value — keeps state alive
        auto request = std::make_unique<PgppRequest>();
        request->task = [state, handle](PgppConnection* conn) mutable {
            if (conn) {
                state->result = std::apply(
                    [&](const auto&... a) -> bool { return conn->execPrepared(state->statement, a...); },
                    state->args);
            }
            handle.resume();
        };
        return state->db.enqueueRaw(std::move(request));
    }

    std::optional<bool> await_resume() noexcept { return m_state->result; }

private:
    struct State {
        State(PgppPool& d, std::string s, Ts... a)
            : db(d), statement(std::move(s)), args(std::move(a)...) {}
        PgppPool&           db;
        std::string         statement;
        std::tuple<Ts...>   args;
        std::optional<bool> result;
    };
    std::shared_ptr<State> m_state;
};

// co_await coQuery<RowTuple>(db, "stmt", args...)
//   -> std::pair<std::optional<bool>, std::vector<RowTuple>>
template<typename RowTuple, typename... Ts>
class DbResultAwaitable
{
public:
    using Rows = std::vector<RowTuple>;

    DbResultAwaitable(PgppPool& db, std::string statement, Ts... args)
        : m_state(std::make_shared<State>(db, std::move(statement), std::move(args)...))
    {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto state = m_state;                // captured by value — keeps state alive
        auto request = std::make_unique<PgppRequest>();
        request->task = [state, handle](PgppConnection* conn) mutable {
            if (conn) {
                state->result = std::apply(
                    [&](const auto&... a) -> bool {
                        return conn->execPrepared(state->statement, state->rows, a...);
                    }, state->args);
            }
            handle.resume();
        };
        return state->db.enqueueRaw(std::move(request));
    }

    std::pair<std::optional<bool>, Rows> await_resume() noexcept
    {
        return { m_state->result, std::move(m_state->rows) };
    }

private:
    struct State {
        State(PgppPool& d, std::string s, Ts... a)
            : db(d), statement(std::move(s)), args(std::move(a)...) {}
        PgppPool&           db;
        std::string         statement;
        std::tuple<Ts...>   args;
        std::optional<bool> result;
        Rows                rows;
    };
    std::shared_ptr<State> m_state;
};

// Factory helpers
template<typename... Ts>
auto coExec(PgppPool& db, std::string statement, Ts&&... args)
{
    return DbExecAwaitable<std::decay_t<Ts>...>(db, std::move(statement), std::forward<Ts>(args)...);
}

template<typename RowTuple, typename... Ts>
auto coQuery(PgppPool& db, std::string statement, Ts&&... args)
{
    return DbResultAwaitable<RowTuple, std::decay_t<Ts>...>(db, std::move(statement), std::forward<Ts>(args)...);
}

// Backward compatibility
template<typename... Ts>
auto coExecPrepared(PgppPool& db, std::string statement, Ts&&... args)
{
    return coExec(db, std::move(statement), std::forward<Ts>(args)...);
}

template<typename RowTuple, typename... Ts>
auto coExecPreparedWithResult(PgppPool& db, std::string statement, Ts&&... args)
{
    return coQuery<RowTuple>(db, std::move(statement), std::forward<Ts>(args)...);
}
