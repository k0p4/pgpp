# pgpp - C++20 PostgreSQL Connection Pool

## Project Identity

pgpp is a C++20 thread-safe PostgreSQL connection pool library built on libpq. It provides
synchronous, future-based, callback-based, and coroutine-based APIs for prepared statements,
raw SQL, and transactions. Designed for consumption via CMake FetchContent.

**License:** GPL-3.0-or-later
**Consumed via:** CMake FetchContent by downstream projects

## Quick Reference

| Action | Command |
|---|---|
| **Build** | `cd build && cmake .. && cmake --build .` |
| **Build (Ninja)** | `cd build && cmake -G Ninja .. && ninja` |
| **Run unit tests** | `cmake --build build --target pgpp_unit_tests && ctest --test-dir build` |

## Project Structure

```
pgpp/
  include/pgpp/
    pgpp.h                # PgppPool, PgppConnectionInfo, PgppRequest, pool template impls
    pgpp_connection.h     # PgppConnection, Statement, pg:: OIDs, type converters, exec template impls
    pgpp_coroutines.h     # FireAndForget, DbExecAwaitable, DbResultAwaitable, coExec, coQuery
  src/
    pgpp.cpp              # PgppPool implementation
    pgpp_connection.cpp   # PgppConnection implementation
    pgpp_log.h            # Compile-time logging (alog / stderr / no-op)
  docs/
    SPECIFICATION.md      # Full API specification with requirements (REQ-PGPP-NNN)
    TESTING_ROADMAP.md    # Test plan: unit tests and integration tests
    usage.md              # Usage examples
  tests/                  # Planned test directory
  CMakeLists.txt
```

## Build System

- **CMake 3.16+**, C++20 required
- Static library target: `pgpp`
- Public dependency: `PostgreSQL::PostgreSQL` (via `find_package`)
- Optional: `alog` logging library (auto-detected, enables `PGPP_USE_ALOG`)
- Supports PostgreSQL 13-17

## Key Classes

### PgppPool (thread-safe)
Connection pool with worker-thread-per-connection model. Main entry point for applications.
- `initialize(PgppConnectionInfo, poolSize)` / `shutdown()`
- `prepareStatement(Statement)` -- registers prepared statements on all connections
- `execSync` / `querySync` -- blocking convenience wrappers
- `execAsync` / `queryAsync` -- return `std::future`
- `exec` / `query` -- callback-based, fires on worker thread
- `transaction(work)` -- auto BEGIN/COMMIT/ROLLBACK
- `execRawSync` / `execRawAsync` -- non-prepared SQL

### PgppConnection (NOT thread-safe)
Single connection wrapper. Used internally by pool workers. Direct use is for testing only.
- `open` / `close` / `reset` / `isOpen` / `lastError`
- `prepare` / `isPrepared`
- `execRaw` / `execPrepared` (with and without result vectors)

### Coroutines (pgpp_coroutines.h)
- `FireAndForget` -- fire-and-forget coroutine return type
- `coExec(db, name, args...)` -- co_await exec
- `coQuery<RowTuple>(db, name, args...)` -- co_await query with typed results

## Specifications

- **[docs/SPECIFICATION.md](docs/SPECIFICATION.md)** -- Full API spec with REQ-PGPP-NNN requirements
- **[docs/TESTING_ROADMAP.md](docs/TESTING_ROADMAP.md)** -- Unit and integration test plan

## Key Conventions

- **Logging:** Compile-time selectable via `PGPP_USE_ALOG` or `PGPP_USE_STDERR`; defaults to no-op
- **Threading:** Pool is thread-safe (mutex + condition_variable + atomics). Connection is NOT thread-safe.
- **Type conversions:** string, int, int16_t, int64_t, uint32_t, double, float, bool. NULL becomes C++ default value.
- **OIDs:** Use `pg::` namespace constants (e.g., `pg::TEXT`, `pg::INT4`), not legacy macros
- **Parameters:** All query parameters are passed as text-format strings (`.c_str()`)
- **Error handling:** Auto-reconnect via `PQreset` on connection loss. Futures return `nullopt` on shutdown.

## Testing

Tests are planned but not yet implemented. See [docs/TESTING_ROADMAP.md](docs/TESTING_ROADMAP.md).

- **Unit tests:** Type conversions, connection string building, pool state machine (no database needed)
- **Integration tests:** Full CRUD, pool concurrency, transactions, coroutines (require PostgreSQL)
- **Framework:** GoogleTest or Catch2 (TBD)

## Downstream Usage

Designed for integration via CMake FetchContent. Downstream projects link against the
`pgpp` target, which transitively provides `PostgreSQL::PostgreSQL` includes and libraries.
