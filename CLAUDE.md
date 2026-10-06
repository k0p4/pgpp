# pgpp - C++23 PostgreSQL Connection Pool

## Project Identity

pgpp is a C++23 thread-safe PostgreSQL connection pool library built on libpq. It provides
synchronous, future-based, callback-based, and coroutine-based APIs for prepared statements,
raw SQL, and transactions. Designed for consumption via CMake FetchContent.

**License:** GPL-3.0-or-later
**Consumed via:** CMake FetchContent by downstream projects

## Quick Reference (Standalone Development)

Prerequisites: C++23 compiler, CMake 3.20+, `VCPKG_ROOT` environment variable.

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
      test_config.h           # Test configuration constants, getTestConnectionInfo()
      docker_fixture.h        # DockerPostgresEnvironment (auto-manage PostgreSQL container)
      integration_fixture.h   # PgppIntegrationTest, PgppConnectionTest fixtures
    unit/                     # Unit tests (no database needed), numeric-prefixed
    integration/
      main.cpp                # Custom main with Docker environment registration
      test_*.cpp              # Integration test files, numeric-prefixed
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

- **CMake 3.20+**, C++23 required (`std::expected`)
- Static library target: `pgpp`
- Public dependency: `PostgreSQL::PostgreSQL` (via `find_package`)
- Optional: `alog` logging library (auto-detected, enables `PGPP_USE_ALOG`)
- Supports PostgreSQL 13-17

## Key Classes

### PgppPool (thread-safe)
Free-list of connections handed out as RAII leases (`acquire()` → `Lease`), plus an executor
(`poolSize` threads, one queue) for the async APIs. Main entry point for applications.
- `initialize(PgppConnectionInfo, poolSize)` / `shutdown()`
- `prepareStatement(Statement)` -- registers a statement; each connection prepares it on its next acquisition
- `acquire()` -- a leased connection for the calling thread (waits up to `acquireTimeout`)
- `execSync` / `querySync` / `execRawSync` / `transactionSync` -- run on the calling thread with their own lease; never use the executor
- `execAsync` / `queryAsync` -- return `std::future`; run on the executor
- `exec` / `query` -- callback-based, fires on an executor thread after the connection was returned
- `transaction(work)` -- BEGIN, `work(conn)` (returns void or bool), COMMIT; ROLLBACK and `false` if a statement failed, `work` returned false or threw
- `execRawSync` / `execRawAsync` -- non-prepared SQL

### PgppConnection (NOT thread-safe)
Single connection wrapper. Reached through a `PgppPool::Lease`; direct construction is for testing only.
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

## Documentation Maintenance

When changing API, tests, or build system — update the corresponding docs:
- **[docs/SPECIFICATION.md](docs/SPECIFICATION.md)** — update when API signatures, requirements, or behavior change
- **[docs/TESTING_ROADMAP.md](docs/TESTING_ROADMAP.md)** — update when tests are added, removed, or restructured
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — update when build instructions, prerequisites, or project layout change

## Key Conventions

- **Logging:** Compile-time selectable via `PGPP_USE_ALOG` or `PGPP_USE_STDERR`; defaults to no-op
- **Threading:** Pool is thread-safe; a connection is used only by the holder of its lease, on the holder's thread. Sync API = acquire on the calling thread (no executor, safe from callbacks); async/callback/coroutine/`transaction()` = executor task that acquires, runs, returns the lease, then completes. Inside `transaction()` work use the given connection (a pool call there takes a second connection). Never block on a future inside a callback. `shutdown()` from an executor thread is non-blocking; never destroy the pool from one. A returned connection is rolled back if left in a transaction (REQ-PGPP-072). The whole suite runs under ThreadSanitizer in CI. Connection is NOT thread-safe.
- **Result decoding:** results are fetched in binary format (REQ-PGPP-062) and decoded by column OID into string / any integral type / double / float / bool, range-checked. No text parsing. Non-text columns read into `std::string` need `::text` in SQL; `numeric` needs `::int8`/`::float8`. NULL becomes the C++ default value.
- **OIDs:** Use `pg::` namespace constants (e.g., `pg::TEXT`, `pg::INT4`), not legacy macros
- **Parameters:** All query parameters are passed as text-format strings (`.c_str()`); only results are binary
- **Error handling:** No exception leaves the library (REQ-PGPP-061): every public function is `noexcept`, everything that can throw inside (allocation, argument copies, thread/mutex errors, user callbacks) is caught and turned into the failure value (`false`, `nullopt`, an already-resolved or invalid future, an immediately-completing awaitable). Auto-reconnect via `PQreset` on connection loss. Futures return `nullopt` on shutdown.

## Testing

- **Framework:** GoogleTest v1.14.0 (fetched via FetchContent)
- **Unit tests:** Binary result decoding, connection string building, pool state machine, no-exceptions guarantees (no database needed)
- **Integration tests:** Full CRUD, pool concurrency, transactions, coroutines (require PostgreSQL)
- **Docker fixture:** Integration tests auto-manage a PostgreSQL container via Docker CLI
  - Set `PGPP_SKIP_DOCKER=1` to skip Docker management (e.g. when PostgreSQL is already running)
  - Container: `pgpp-test-pg-7f3a` on port 15432, image `postgres:16-alpine`

## Downstream Usage

Designed for integration via CMake FetchContent. Downstream projects link against the
`pgpp` target, which transitively provides `PostgreSQL::PostgreSQL` includes and libraries.

## Contributing & Building

See @CONTRIBUTING.md for prerequisites, build instructions, and testing guide.
