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

// If PGPP_USE_ALOG is defined, uses alog. Otherwise no-op (zero overhead).
// Define PGPP_USE_STDERR for simple stderr output without alog.

#if defined(PGPP_USE_ALOG)
    #include <alog/all.h>
    #define PGPP_DEFINE_LOG_MODULE(name) DEFINE_ALOGGER_MODULE_NS(name)
    #define PGPP_LOGV LOGV
    #define PGPP_LOGD LOGD
    #define PGPP_LOGW LOGW
    #define PGPP_LOGE LOGE
#elif defined(PGPP_USE_STDERR)
    #include <iostream>
    #define PGPP_DEFINE_LOG_MODULE(name)
    struct PgppStderrLog {
        ~PgppStderrLog() { std::cerr << std::endl; }
        template<typename T> PgppStderrLog& operator<<(const T& v) { std::cerr << v; return *this; }
    };
    #define PGPP_LOGV PgppStderrLog()
    #define PGPP_LOGD PgppStderrLog()
    #define PGPP_LOGW PgppStderrLog()
    #define PGPP_LOGE PgppStderrLog()
#else
    #define PGPP_DEFINE_LOG_MODULE(name)
    struct PgppNullLog {
        template<typename T> PgppNullLog& operator<<(const T&) { return *this; }
    };
    #define PGPP_LOGV PgppNullLog()
    #define PGPP_LOGD PgppNullLog()
    #define PGPP_LOGW PgppNullLog()
    #define PGPP_LOGE PgppNullLog()
#endif
