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

#include "pgpp.h"

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

// co_await coExec(db, "stmt", args...) -> std::optional<bool>
template<typename... Ts>
class DbExecAwaitable
{
public:
    DbExecAwaitable(PgppPool& db, std::string statement, Ts... args)
        : m_db(db), m_statement(std::move(statement)), m_args(std::move(args)...) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto request = std::make_unique<PgppRequest>();
        request->task = [this, handle](PgppConnection* conn) mutable {
            if (conn) {
                m_result = std::apply(
                    [&](const auto&... a) -> bool { return conn->execPrepared(m_statement, a...); },
                    m_args);
            }
            handle.resume();
        };
        return m_db.enqueueRaw(std::move(request));
    }

    std::optional<bool> await_resume() noexcept { return m_result; }

private:
    PgppPool&           m_db;
    std::string         m_statement;
    std::tuple<Ts...>   m_args;
    std::optional<bool> m_result;
};

// co_await coQuery<RowTuple>(db, "stmt", args...)
//   -> std::pair<std::optional<bool>, std::vector<RowTuple>>
template<typename RowTuple, typename... Ts>
class DbResultAwaitable
{
public:
    using Rows = std::vector<RowTuple>;

    DbResultAwaitable(PgppPool& db, std::string statement, Ts... args)
        : m_db(db), m_statement(std::move(statement)), m_args(std::move(args)...) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto request = std::make_unique<PgppRequest>();
        request->task = [this, handle](PgppConnection* conn) mutable {
            if (conn) {
                m_result = std::apply(
                    [&](const auto&... a) -> bool {
                        return conn->execPrepared(m_statement, m_rows, a...);
                    }, m_args);
            }
            handle.resume();
        };
        return m_db.enqueueRaw(std::move(request));
    }

    std::pair<std::optional<bool>, Rows> await_resume() noexcept
    {
        return { m_result, std::move(m_rows) };
    }

private:
    PgppPool&           m_db;
    std::string         m_statement;
    std::tuple<Ts...>   m_args;
    std::optional<bool> m_result;
    Rows                m_rows;
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
