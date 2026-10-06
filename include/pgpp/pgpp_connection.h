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

#include <array>          // std::array (column OIDs; valid for zero columns)
#include <bit>            // std::bit_cast
#include <cfloat>         // FLT_MAX
#include <cmath>          // std::isfinite, std::fabs
#include <concepts>       // std::integral, std::floating_point
#include <cstddef>        // size_t, std::ptrdiff_t
#include <cstdint>        // int16_t, uint32_t, int64_t
#include <expected>       // std::expected, std::unexpected
#include <memory>         // std::unique_ptr
#include <optional>       // std::optional (NULL parameters and columns)
#include <string>         // std::string, std::char_traits
#include <string_view>
#include <tuple>
#include <type_traits>    // std::is_same_v, std::false_type
#include <utility>        // std::move, std::in_range, std::index_sequence
#include <vector>

#include <libpq-fe.h>

typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

namespace pg {
    constexpr uint32_t BOOL         = 16;
    constexpr uint32_t BYTEA        = 17;
    constexpr uint32_t CHAR         = 18;
    constexpr uint32_t NAME         = 19;
    constexpr uint32_t INT8         = 20;
    constexpr uint32_t INT2         = 21;
    constexpr uint32_t INT4         = 23;
    constexpr uint32_t TEXT         = 25;
    constexpr uint32_t OID          = 26;
    constexpr uint32_t JSON         = 114;
    constexpr uint32_t XML          = 142;
    constexpr uint32_t FLOAT4       = 700;
    constexpr uint32_t FLOAT8       = 701;
    constexpr uint32_t UNKNOWN      = 705;
    constexpr uint32_t BPCHAR       = 1042;
    constexpr uint32_t VARCHAR      = 1043;
    constexpr uint32_t DATE         = 1082;
    constexpr uint32_t TIME         = 1083;
    constexpr uint32_t TIMESTAMP    = 1114;
    constexpr uint32_t TIMESTAMPTZ  = 1184;
    constexpr uint32_t TIMETZ       = 1266;
    constexpr uint32_t NUMERIC      = 1700;
    constexpr uint32_t CSTRING      = 2275;
    constexpr uint32_t JSONB        = 3802;
}

// Legacy macros — prefer pg:: namespace constants directly.
// Define PGPP_ENABLE_OID_MACROS before including to enable.
#ifdef PGPP_ENABLE_OID_MACROS
#define BOOLOID         pg::BOOL
#define BYTEAOID        pg::BYTEA
#define CHAROID         pg::CHAR
#define NAMEOID         pg::NAME
#define INT8OID         pg::INT8
#define INT2OID         pg::INT2
#define INT4OID         pg::INT4
#define TEXTOID         pg::TEXT
#define OIDOID          pg::OID
#define JSONOID         pg::JSON
#define XMLOID          pg::XML
#define FLOAT4OID       pg::FLOAT4
#define FLOAT8OID       pg::FLOAT8
#define UNKNOWNOID      pg::UNKNOWN
#define BPCHAROID       pg::BPCHAR
#define VARCHAROID      pg::VARCHAR
#define DATEOID         pg::DATE
#define TIMEOID         pg::TIME
#define TIMESTAMPOID    pg::TIMESTAMP
#define TIMESTAMPTZOID  pg::TIMESTAMPTZ
#define TIMETZOID       pg::TIMETZ
#define NUMERICOID      pg::NUMERIC
#define CSTRINGOID      pg::CSTRING
#define JSONBOID        pg::JSONB
#endif

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

    // Every function is noexcept (REQ-PGPP-061); failures are return values.
    bool open(const std::string& connectionInfo) noexcept;
    bool isOpen() const noexcept;
    void reset() noexcept;
    void close() noexcept;

    std::string lastError() const noexcept;   // empty if it cannot be copied

    bool prepare(const Statement& statement) noexcept;
    bool isPrepared(const std::string& statementName) noexcept;
    bool execRaw(const std::string& sql) noexcept;

    // Parameters are std::string (or anything with .c_str() and .size()), or
    // std::optional of one where nullopt is SQL NULL (REQ-PGPP-075). A value
    // with an embedded NUL is refused (REQ-PGPP-074). Result columns are
    // std::string, any integral type, float, double, bool, or std::optional of
    // one of those where NULL becomes nullopt.
    template<typename... Ts, typename... TAs>
    bool execPrepared(const std::string& statement, std::vector<std::tuple<TAs...>>& result, const Ts&... args) noexcept;

    // Without results: rows the statement returns (SELECT, INSERT ... RETURNING)
    // are discarded (REQ-PGPP-073).
    template<typename... Ts>
    bool execPrepared(const std::string& statement, const Ts&... args) noexcept;

    PGconn* connection() noexcept { return m_connection; }

private:
    void logTemplateError(const std::string& statement, int status) noexcept;
    void logParamError(const std::string& statement) noexcept;
    void logConversionError(const std::string& statement, int row) noexcept;
    void logColumnCountError(const std::string& statement, int columns, int requested) noexcept;
    PGconn* m_connection { nullptr };
};

// ── Template implementations ─────────────────────────────────────────────────

struct PQResultDeleter {
    void operator()(PGresult* r) const { if (r) PQclear(r); }
};
using PQResultPtr = std::unique_ptr<PGresult, PQResultDeleter>;

namespace Internal {
namespace Details {

// Results are requested in binary format (REQ-PGPP-062): a cell is the
// PostgreSQL wire encoding of its column type, never text to parse. Each
// decoder checks the column's type OID against the requested C++ type and
// reports failure through std::expected; pgpp never throws (REQ-PGPP-061).
enum class ConversionError {
    TypeMismatch,   // the column's PostgreSQL type cannot be read into the requested C++ type
    OutOfRange,     // the value does not fit the requested C++ type
    Malformed       // the cell is not the size its type's binary encoding requires
};

template<typename T>
using Converted = std::expected<T, ConversionError>;

template<typename T>
struct IsOptional : std::false_type {};
template<typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

// The text-format pointer libpq gets for one parameter: nullptr is SQL NULL.
template<typename T>
const char* paramValue(const T& arg) noexcept
{
    if constexpr (IsOptional<T>::value) {
        return arg.has_value() ? paramValue(*arg) : nullptr;
    } else {
        return arg.c_str();
    }
}

// A text parameter is sent as a NUL-terminated C string, so everything after
// an embedded NUL would be silently dropped (CWE-158): such a value is refused
// instead (REQ-PGPP-074). Detected for any argument that knows its size.
template<typename T>
bool paramHasEmbeddedNul(const T& arg) noexcept
{
    if constexpr (IsOptional<T>::value) {
        return arg.has_value() && paramHasEmbeddedNul(*arg);
    } else if constexpr (requires { arg.size(); }) {
        return std::char_traits<char>::length(arg.c_str()) != static_cast<size_t>(arg.size());
    } else {
        return false;
    }
}

// Column types whose binary encoding is the text itself (client encoding, no terminator).
inline bool isTextualType(Oid type) noexcept
{
    switch (type) {
    case pg::CHAR:
    case pg::NAME:
    case pg::TEXT:
    case pg::JSON:
    case pg::XML:
    case pg::UNKNOWN:
    case pg::BPCHAR:
    case pg::VARCHAR:
    case pg::CSTRING:
    case pg::JSONB:
        return true;
    default:
        return false;
    }
}

// Network byte order to host order, up to 8 bytes.
inline uint64_t loadBigEndian(std::string_view bytes) noexcept
{
    uint64_t value = 0;
    for (const char byte : bytes) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

// INT2 / INT4 / INT8 / OID cells as one 64-bit value; every one of them fits.
inline Converted<int64_t> decodeWideInteger(Oid type, std::string_view bytes) noexcept
{
    size_t width = 0;
    switch (type) {
    case pg::INT2:
        width = 2;
        break;
    case pg::INT4:
    case pg::OID:
        width = 4;
        break;
    case pg::INT8:
        width = 8;
        break;
    default:
        return std::unexpected(ConversionError::TypeMismatch);
    }

    if (bytes.size() != width) {
        return std::unexpected(ConversionError::Malformed);
    }

    const uint64_t raw = loadBigEndian(bytes);
    switch (type) {
    case pg::INT2:
        return static_cast<int16_t>(raw);
    case pg::INT4:
        return static_cast<int32_t>(raw);
    case pg::OID:
        return static_cast<int64_t>(static_cast<uint32_t>(raw));
    default:
        return static_cast<int64_t>(raw);
    }
}

template<typename T>
    requires std::integral<T> && (!std::is_same_v<T, bool>)
Converted<T> decodeIntegral(Oid type, std::string_view bytes) noexcept
{
    const auto wide = decodeWideInteger(type, bytes);
    if (!wide.has_value()) {
        return std::unexpected(wide.error());
    } else if (!std::in_range<T>(*wide)) {
        // 40000 into int16_t, -1 or 5000000000 into uint32_t: never a silent wrap.
        return std::unexpected(ConversionError::OutOfRange);
    } else {
        return static_cast<T>(*wide);
    }
}

template<typename T>
    requires std::floating_point<T>
Converted<T> decodeFloating(Oid type, std::string_view bytes) noexcept
{
    if (type == pg::FLOAT4) {
        if (bytes.size() != 4) {
            return std::unexpected(ConversionError::Malformed);
        } else {
            return static_cast<T>(std::bit_cast<float>(static_cast<uint32_t>(loadBigEndian(bytes))));
        }
    } else if (type == pg::FLOAT8) {
        if (bytes.size() != 8) {
            return std::unexpected(ConversionError::Malformed);
        }

        const double value = std::bit_cast<double>(loadBigEndian(bytes));
        if constexpr (std::is_same_v<T, float>) {
            if (std::isfinite(value) && std::fabs(value) > static_cast<double>(FLT_MAX)) {
                return std::unexpected(ConversionError::OutOfRange);
            }
        }
        return static_cast<T>(value);
    } else {
        // Integer columns read into a floating-point type (exact below 2^53).
        const auto wide = decodeWideInteger(type, bytes);
        if (!wide.has_value()) {
            return std::unexpected(wide.error());
        } else {
            return static_cast<T>(*wide);
        }
    }
}

template<typename T>
Converted<T> decodeBinary(Oid type, std::string_view bytes)
{
    if constexpr (IsOptional<T>::value) {
        // std::optional<U>: a non-NULL cell decodes as U (NULL never gets here,
        // see fillPQValue).
        auto inner = decodeBinary<typename T::value_type>(type, bytes);
        if (!inner.has_value()) {
            return std::unexpected(inner.error());
        } else {
            return T(std::move(*inner));
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!isTextualType(type)) {
            // int, timestamp, uuid, numeric, ...: cast to ::text in the SQL instead.
            return std::unexpected(ConversionError::TypeMismatch);
        } else if (type == pg::JSONB) {
            // jsonb binary = one version byte (1) followed by the JSON text.
            if (bytes.empty() || bytes.front() != 1) {
                return std::unexpected(ConversionError::Malformed);
            } else {
                return std::string(bytes.substr(1));
            }
        } else {
            return std::string(bytes);
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (type != pg::BOOL) {
            return std::unexpected(ConversionError::TypeMismatch);
        } else if (bytes.size() != 1) {
            return std::unexpected(ConversionError::Malformed);
        } else {
            return bytes.front() != 0;
        }
    } else if constexpr (std::floating_point<T>) {
        return decodeFloating<T>(type, bytes);
    } else {
        static_assert(std::integral<T>, "unsupported result column type");
        // std::in_range is defined for the standard integer types only; a
        // character type would fail deep inside <utility>.
        static_assert(!std::is_same_v<T, char> && !std::is_same_v<T, wchar_t>
                      && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t>
                      && !std::is_same_v<T, char32_t>,
                      "character types are not result column types: use std::string or int16_t");
        return decodeIntegral<T>(type, bytes);
    }
}

// NULL leaves the default value (nullopt for a std::optional column,
// REQ-PGPP-008/075). Returns false if the cell could not be decoded. `type` is
// the column's OID, looked up once per result rather than per cell.
template<typename T>
bool fillPQValue(PGresult* result, int row, int col, Oid type, T& value)
{
    if (PQgetisnull(result, row, col)) {
        if constexpr (IsOptional<T>::value) {
            value.reset();
        }
        return true;
    }

    const std::string_view bytes(PQgetvalue(result, row, col),
                                 static_cast<size_t>(PQgetlength(result, row, col)));
    auto decoded = decodeBinary<T>(type, bytes);
    if (!decoded.has_value()) {
        return false;
    } else {
        value = std::move(*decoded);
        return true;
    }
}

// Returns false at the first column that fails to decode.
template<typename... TAs, size_t... Is>
bool fillTupleFromPQValues([[maybe_unused]] PGresult* result, [[maybe_unused]] int row,
                           [[maybe_unused]] const Oid* types, [[maybe_unused]] std::tuple<TAs...>& tuple,
                           std::index_sequence<Is...>)
{
    // For std::tuple<> the pack is empty and every parameter is unused.
    return (fillPQValue(result, row, static_cast<int>(Is), types[Is], std::get<Is>(tuple)) && ...);
}

} // namespace Details
} // namespace Internal

template<typename... Ts>
bool PgppConnection::execPrepared(const std::string& statement, const Ts&... args) noexcept
{
    constexpr int size = sizeof...(args);
    if ((Internal::Details::paramHasEmbeddedNul(args) || ... || false)) [[unlikely]] {
        logParamError(statement);
        return false;
    }
    const char* paramValuesArr[] = { Internal::Details::paramValue(args)..., nullptr };
    const char** paramValues = size > 0 ? paramValuesArr : nullptr;

    PQResultPtr queryResult(PQexecPrepared(m_connection, statement.c_str(), size, paramValues, NULL, NULL, 0));
    if (!queryResult) return false;

    // The statement ran: a command (PGRES_COMMAND_OK) or one that returned rows
    // nobody asked for (PGRES_TUPLES_OK: SELECT, INSERT ... RETURNING), REQ-PGPP-073.
    if (auto status = PQresultStatus(queryResult.get());
        status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) [[unlikely]] {
        logTemplateError(statement, status);
        return false;
    }
    return true;
}

template<typename... Ts, typename... TAs>
bool PgppConnection::execPrepared(const std::string& statement, std::vector<std::tuple<TAs...>>& result, const Ts&... args) noexcept
{
    constexpr int size = sizeof...(args);
    if ((Internal::Details::paramHasEmbeddedNul(args) || ... || false)) [[unlikely]] {
        logParamError(statement);
        return false;
    }
    const char* paramValuesArr[] = { Internal::Details::paramValue(args)..., nullptr };
    const char** paramValues = size > 0 ? paramValuesArr : nullptr;

    // Parameters are text (REQ-PGPP-016); results are requested in binary (REQ-PGPP-062).
    PQResultPtr queryResult(PQexecPrepared(m_connection, statement.c_str(), size, paramValues, NULL, NULL, 1));
    if (!queryResult) {
        return false;
    }

    if (auto status = PQresultStatus(queryResult.get()); status != PGRES_TUPLES_OK) [[unlikely]] {
        logTemplateError(statement, status);
        return false;
    }

    constexpr size_t columns = sizeof...(TAs);
    if (PQnfields(queryResult.get()) < static_cast<int>(columns)) [[unlikely]] {
        logColumnCountError(statement, PQnfields(queryResult.get()), static_cast<int>(columns));
        return false;
    }

    // std::array, not a C array: `std::tuple<>` makes `columns` 0, and a
    // zero-length C array is ill-formed (MSVC C2466).
    std::array<Oid, columns> types{};
    for (size_t col = 0; col < columns; col++) {
        types[col] = PQftype(queryResult.get(), static_cast<int>(col));
    }

    const auto initialSize = result.size();
    try {
        const int rows = PQntuples(queryResult.get());
        result.reserve(initialSize + static_cast<size_t>(rows));
        for (int idx = 0; idx < rows; idx++) {
            std::tuple<TAs...> row;
            if (!Internal::Details::fillTupleFromPQValues(queryResult.get(), idx, types.data(), row, std::make_index_sequence<columns>{})) [[unlikely]] {
                // A cell that cannot be decoded into the requested type (wrong column
                // type, or bigint > INT_MAX into int): fail the whole query rather than
                // hand back a partial result (REQ-PGPP-057).
                result.erase(result.begin() + static_cast<std::ptrdiff_t>(initialSize), result.end());
                logConversionError(statement, idx);
                return false;
            }
            result.push_back(std::move(row));
        }
        return true;
    } catch (...) {
        // Allocation failed while growing the result: same contract, no partial rows.
        result.erase(result.begin() + static_cast<std::ptrdiff_t>(initialSize), result.end());
        logTemplateError(statement, -1);
        return false;
    }
}
