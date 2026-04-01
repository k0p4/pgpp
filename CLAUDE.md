# pgpp - C++20 PostgreSQL Connection Pool

## Project Identity

pgpp is a C++20 thread-safe PostgreSQL connection pool library built on libpq. It provides
synchronous, future-based, callback-based, and coroutine-based APIs for prepared statements,
raw SQL, and transactions. Designed for consumption via CMake FetchContent.

**License:** GPL-3.0-or-later
**Consumed via:** CMake FetchContent by downstream projects

## Quick Reference (Standalone Development)

Prerequisites: C++20 compiler, CMake 3.16+, `VCPKG_ROOT` environment variable.

```bash
python build_and_test.py              # full pipeline: configure + build + test
python build_and_test.py --unit-only  # unit tests only (no Docker needed)
python build_and_test.py --skip-tests # only configure and build
```

Or use CMake directly:

| Action | Command |
|---|---|
| **Configure** | `cmake --preset dev-debug` |
| **Build** | `cmake --build --preset dev-debug` |
| **Run all tests** | `ctest --preset dev-debug` |

vcpkg manifest (`vcpkg.json`) installs libpq automatically during configure.

## Usage Scenarios

- **Standalone dev**: CMake presets + vcpkg — libpq is provided automatically
- **FetchContent consumer**: Consumer's project provides `PostgreSQL::PostgreSQL` target; vcpkg.json and presets are ignored

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
  tests/
    common/
      test_config.h       # Test configuration constants, getTestConnectionInfo()
      docker_fixture.h    # DockerPostgresEnvironment (auto-manage PostgreSQL container)
    unit/                 # Unit tests (no database needed)
    integration/
      main.cpp            # Custom main with Docker environment registration
      test_fixture.h      # Integration test fixtures
      test_*.cpp           # Integration test files
  docs/
    SPECIFICATION.md      # Full API specification with requirements (REQ-PGPP-NNN)
    TESTING_ROADMAP.md    # Test plan: unit tests and integration tests
    usage.md              # Usage examples
  build_and_test.py       # Cross-platform build & test script
  CMakePresets.json       # Dev presets (vcpkg + tests enabled)
  vcpkg.json              # vcpkg manifest (libpq dependency)
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

- **Framework:** GoogleTest v1.14.0 (fetched via FetchContent)
- **Unit tests:** Type conversions, connection string building, pool state machine (no database needed)
- **Integration tests:** Full CRUD, pool concurrency, transactions, coroutines (require PostgreSQL)
- **Docker fixture:** Integration tests auto-manage a PostgreSQL container via Docker CLI
  - Set `PGPP_SKIP_DOCKER=1` to skip Docker management (e.g. when PostgreSQL is already running)
  - Container: `pgpp-test-pg-7f3a` on port 15432, image `postgres:16-alpine`

## Downstream Usage

Designed for integration via CMake FetchContent. Downstream projects link against the
`pgpp` target, which transitively provides `PostgreSQL::PostgreSQL` includes and libraries.
