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

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <tuple>

#ifdef _WIN32
#include <libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif

typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

namespace pg {
    constexpr uint32_t BYTEA        = 17;
    constexpr uint32_t CHAR         = 18;
    constexpr uint32_t INT8         = 20;
    constexpr uint32_t INT2         = 21;
    constexpr uint32_t INT4         = 23;
    constexpr uint32_t TEXT         = 25;
    constexpr uint32_t FLOAT4       = 700;
    constexpr uint32_t FLOAT8       = 701;
    constexpr uint32_t VARCHAR      = 1043;
    constexpr uint32_t DATE         = 1082;
    constexpr uint32_t TIME         = 1083;
    constexpr uint32_t TIMESTAMP    = 1114;
    constexpr uint32_t TIMESTAMPTZ  = 1184;
    constexpr uint32_t TIMETZ       = 1266;
}

#define BYTEAOID        pg::BYTEA
#define CHAROID         pg::CHAR
#define INT8OID         pg::INT8
#define INT2OID         pg::INT2
#define INT4OID         pg::INT4
#define TEXTOID         pg::TEXT
#define FLOAT4OID       pg::FLOAT4
#define FLOAT8OID       pg::FLOAT8
#define VARCHAROID      pg::VARCHAR
#define DATEOID         pg::DATE
#define TIMEOID         pg::TIME
#define TIMESTAMPOID    pg::TIMESTAMP
#define TIMESTAMPTZOID  pg::TIMESTAMPTZ
#define TIMETZOID       pg::TIMETZ

struct Statement {
    std::string statementName;
    std::string statement;
    std::vector<uint32_t> variables;
};

class PgppConnection final
{
public:
    PgppConnection();
    ~PgppConnection();

    PgppConnection(const PgppConnection&) = delete;
    PgppConnection& operator=(const PgppConnection&) = delete;

    bool open(const std::string& connectionInfo);
    bool isOpen() const;
    void reset();
    void close();

    const std::string lastError() const;

    bool prepare(const Statement& statement);
    bool isPrepared(const std::string& statementName);
    bool execRaw(const std::string& sql);

    template<typename... Ts, typename... TAs>
    bool execPrepared(const std::string& statement, std::vector<std::tuple<TAs...>>& result, const Ts&... args);

    template<typename... Ts>
    bool execPrepared(const std::string& statement, const Ts&... args);

    PGconn* connection() { return m_connection; }

private:
    void logTemplateError(const std::string& statement, int status);
    PGconn* m_connection { nullptr };
};

// ── Template implementations ─────────────────────────────────────────────────

struct PQResultDeleter {
    void operator()(PGresult* r) const { if (r) PQclear(r); }
};
using PQResultPtr = std::unique_ptr<PGresult, PQResultDeleter>;

namespace Internal {
namespace Details {

template<typename T, std::enable_if_t<std::is_same_v<T, std::string>>* = nullptr>
std::string convertPQValue(const char* raw) { return raw; }

template<typename T, std::enable_if_t<std::is_same_v<T, int>>* = nullptr>
int convertPQValue(const char* raw) { return std::stoi(raw); }

template<typename T, std::enable_if_t<std::is_same_v<T, int16_t> && !std::is_same_v<int16_t, int>>* = nullptr>
int16_t convertPQValue(const char* raw) { return static_cast<int16_t>(std::stoi(raw)); }

template<typename T, std::enable_if_t<std::is_same_v<T, int64_t> && !std::is_same_v<int64_t, int>>* = nullptr>
int64_t convertPQValue(const char* raw) { return std::stoll(raw); }

template<typename T, std::enable_if_t<std::is_same_v<T, uint32_t> && !std::is_same_v<uint32_t, int>>* = nullptr>
uint32_t convertPQValue(const char* raw) { return static_cast<uint32_t>(std::stoul(raw)); }

template<typename T, std::enable_if_t<std::is_same_v<T, double>>* = nullptr>
double convertPQValue(const char* raw) { return std::stod(raw); }

template<typename T, std::enable_if_t<std::is_same_v<T, float>>* = nullptr>
float convertPQValue(const char* raw) { return std::stof(raw); }

template<typename T, std::enable_if_t<std::is_same_v<T, bool>>* = nullptr>
bool convertPQValue(const char* raw) { return raw[0] == 't' || raw[0] == 'T' || raw[0] == '1'; }

template<typename T>
void fillPQValue(PGresult* queryResult, int rowIdx, int colIdx, T& value)
{
    if (PQgetisnull(queryResult, rowIdx, colIdx)) return;
    value = convertPQValue<T>(PQgetvalue(queryResult, rowIdx, colIdx));
}

template<typename... TAs, size_t... Is>
void fillTupleFromPQValues(PGresult* result, int row, std::tuple<TAs...>& tuple, std::index_sequence<Is...>)
{
    (fillPQValue(result, row, Is, std::get<Is>(tuple)), ...);
}

} // namespace Details
} // namespace Internal

template<typename... Ts>
bool PgppConnection::execPrepared(const std::string& statement, const Ts&... args)
{
    constexpr auto size = sizeof...(args);
    const char* paramValuesArr[] = { args.c_str()..., nullptr };
    const char** paramValues = size > 0 ? paramValuesArr : nullptr;

    PQResultPtr queryResult(PQexecPrepared(m_connection, statement.c_str(), size, paramValues, NULL, NULL, 0));
    if (!queryResult) return false;

    if (auto status = PQresultStatus(queryResult.get()); status != PGRES_COMMAND_OK) [[unlikely]] {
        logTemplateError(statement, status);
        return false;
    }
    return true;
}

template<typename... Ts, typename... TAs>
bool PgppConnection::execPrepared(const std::string& statement, std::vector<std::tuple<TAs...>>& result, const Ts&... args)
{
    constexpr auto size = sizeof...(args);
    const char* paramValuesArr[] = { args.c_str()..., nullptr };
    const char** paramValues = size > 0 ? paramValuesArr : nullptr;

    PQResultPtr queryResult(PQexecPrepared(m_connection, statement.c_str(), size, paramValues, NULL, NULL, 0));
    if (!queryResult) return false;

    if (auto status = PQresultStatus(queryResult.get()); status != PGRES_TUPLES_OK) [[unlikely]] {
        logTemplateError(statement, status);
        return false;
    }

    int rows = PQntuples(queryResult.get());
    for (int idx = 0; idx < rows; idx++) {
        std::tuple<TAs...> row;
        Internal::Details::fillTupleFromPQValues(queryResult.get(), idx, row, std::make_index_sequence<sizeof...(TAs)>{});
        result.push_back(row);
    }
    return true;
}
