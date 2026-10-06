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

#include <pgpp/pgpp_connection.h>
#include "pgpp_log.h"
PGPP_DEFINE_LOG_MODULE(PgppConnection)

PgppConnection::PgppConnection()
    : m_connection(nullptr)
{}

PgppConnection::~PgppConnection()
{
    close();
}

void PgppConnection::reset() noexcept
{
    if (m_connection) PQreset(m_connection);
}

void PgppConnection::close() noexcept
{
    if (m_connection) {
        PQfinish(m_connection);
        m_connection = nullptr;
    }
}

bool PgppConnection::isOpen() const noexcept
{
    return m_connection != nullptr && PQstatus(m_connection) == CONNECTION_OK;
}

std::string PgppConnection::lastError() const noexcept
{
    try {
        if (m_connection) {
            return PQerrorMessage(m_connection);
        } else {
            return "";
        }
    } catch (...) {
        return "";   // the message could not be copied; nothing else to report
    }
}

bool PgppConnection::open(const std::string& connectionInfo) noexcept
{
    PGPP_LOGV << __func__;
    if (isOpen()) return true;

    m_connection = PQconnectdb(connectionInfo.c_str());
    if (!isOpen()) {
        PGPP_LOGE << "Could not open db! " << lastError();
        PQfinish(m_connection);
        m_connection = nullptr;
        return false;
    }
    return true;
}

bool PgppConnection::prepare(const Statement& statement) noexcept
{
    PGPP_LOGV << __func__ << statement.statementName.c_str();
    if (!isOpen()) { PGPP_LOGE << "Connection is not open!"; return false; }

    // Statement::variables holds the OIDs in libpq's own type: no copy, no allocation.
    static_assert(sizeof(Oid) == sizeof(uint32_t) && std::is_unsigned_v<Oid>);
    auto result = PQprepare(m_connection,
                            statement.statementName.c_str(),
                            statement.statement.c_str(),
                            static_cast<int>(statement.variables.size()),
                            reinterpret_cast<const Oid*>(statement.variables.data()));
    if (!result) {
        PGPP_LOGE << "Unable to prepare statement " << statement.statementName << "!";
        return false;
    }
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        PGPP_LOGE << "Unable to prepare statement " << statement.statementName << "! Error: " << lastError();
        PQclear(result);
        return false;
    }
    PQclear(result);
    return true;
}

bool PgppConnection::isPrepared(const std::string& statementName) noexcept
{
    if (!isOpen()) return false;
    PGresult* queryResult = PQdescribePrepared(m_connection, statementName.c_str());
    if (!queryResult) return false;
    bool result = (PQresultStatus(queryResult) == PGRES_COMMAND_OK);
    PQclear(queryResult);
    return result;
}

bool PgppConnection::execRaw(const std::string& sql) noexcept
{
    if (!isOpen()) { PGPP_LOGE << "execRaw: connection is not open!"; return false; }
    PGresult* result = PQexec(m_connection, sql.c_str());
    if (!result) return false;
    ExecStatusType status = PQresultStatus(result);
    PQclear(result);
    return (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
}

void PgppConnection::logTemplateError(const std::string& statement, int status) noexcept
{
    PGPP_LOGE << "Unable to execute query for " << statement << "! Status:" << status;
    PGPP_LOGE << lastError();
}

void PgppConnection::logParamError(const std::string& statement) noexcept
{
    PGPP_LOGE << "Refusing to execute " << statement
              << ": a parameter contains an embedded NUL (text parameters cannot)";
}

void PgppConnection::logColumnCountError(const std::string& statement, int columns, int requested) noexcept
{
    PGPP_LOGE << "Result of " << statement << " has " << columns << " column(s), but "
              << requested << " were requested";
}

void PgppConnection::logConversionError(const std::string& statement, int row) noexcept
{
    PGPP_LOGE << "Unable to decode result of " << statement << " at row " << row
              << ": column type does not match the requested C++ type, or the value does not fit";
}
