# pgpp Testing Roadmap

## Overview

Testing strategy for the pgpp PostgreSQL connection pool library. Tests are divided into
unit tests (no database required) and integration tests (require a running PostgreSQL instance).

**Framework:** GoogleTest or Catch2 (TBD)
**Location:** `tests/` directory

---

## Phase 1: Unit Tests (Priority: High)

No PostgreSQL instance needed. Test pure logic: connection string building, type conversions,
struct construction, pool state machine basics.

### Connection String Builder

| ID | Test | Requirement | Priority |
|---|---|---|---|
| UT-CONN-001 | `buildConnectionString` with all fields populated produces valid libpq string | REQ-PGPP-001, REQ-PGPP-002 | High |
| UT-CONN-002 | `buildConnectionString` with empty `dbname` returns empty string | REQ-PGPP-001 | High |
| UT-CONN-003 | `buildConnectionString` escapes `'` and `\` in password field | REQ-PGPP-002 | High |

**Notes:** `buildConnectionString` is a private method. Tests require either a test helper/friend
class or extracting the escaping logic into a testable free function.

### Type Conversions

| ID | Test | Requirement | Priority |
|---|---|---|---|
| UT-CONV-001 | `convertPQValue<string>` returns raw string unchanged | REQ-PGPP-009 | High |
| UT-CONV-002 | `convertPQValue<int>` parses `"42"` to `42` | REQ-PGPP-009 | High |
| UT-CONV-003 | `convertPQValue<int16_t>` parses `"123"` to `int16_t(123)` | REQ-PGPP-009 | High |
| UT-CONV-004 | `convertPQValue<int64_t>` parses `"9999999999"` to correct `int64_t` | REQ-PGPP-009 | High |
| UT-CONV-005 | `convertPQValue<uint32_t>` parses `"4000000000"` to correct `uint32_t` | REQ-PGPP-009 | High |
| UT-CONV-006 | `convertPQValue<double>` parses `"3.14159"` to approximately `3.14159` | REQ-PGPP-009 | High |
| UT-CONV-007 | `convertPQValue<float>` parses `"2.5"` to `2.5f` | REQ-PGPP-009 | High |
| UT-CONV-008 | `convertPQValue<bool>` returns `true` for `"t"`, `"T"`, `"1"` | REQ-PGPP-009 | High |
| UT-CONV-009 | `convertPQValue<bool>` returns `false` for `"f"`, `"F"`, `"0"` | REQ-PGPP-009 | High |

### Pool State Machine

| ID | Test | Requirement | Priority |
|---|---|---|---|
| UT-POOL-001 | Default constructor sets `m_poolSize` to `hardware_concurrency` or 16 | REQ-PGPP-019 | Medium |
| UT-POOL-002 | Double `initialize` returns `true` (idempotent, without DB this tests the atomic guard only) | REQ-PGPP-019 | Medium |
| UT-POOL-003 | Double `shutdown` does not crash | REQ-PGPP-020 | Medium |
| UT-POOL-004 | `execSync`/`querySync`/`execRawSync` on uninitialized pool do not crash (graceful failure) | REQ-PGPP-028, REQ-PGPP-029 | Medium |
| UT-POOL-005 | `shutdown` then `initialize` works (re-initialization after full shutdown) | REQ-PGPP-020, REQ-PGPP-019 | Medium |

**Notes:** UT-POOL-002 through UT-POOL-005 will fail at the connection creation step without a
database, but should not crash or invoke undefined behavior. The atomic guards and state
transitions are the testable parts.

### Struct Construction

| ID | Test | Requirement | Priority |
|---|---|---|---|
| UT-STMT-001 | `Statement` struct holds `statementName`, `statement`, and `variables` correctly | REQ-PGPP-004, REQ-PGPP-005 | Low |

### OID Constants

| ID | Test | Requirement | Priority |
|---|---|---|---|
| UT-OID-001 | `pg::` namespace OID constants match documented PostgreSQL catalog values | REQ-PGPP-006 | Low |

---

## Phase 2: Integration Tests (Priority: High)

Require a running PostgreSQL instance. Environment variable `PGPP_TEST_DSN` or a local
`test_config.ini` provides connection parameters.

### Prerequisites

- PostgreSQL 15+ running locally or in Docker
- Test database created: `CREATE DATABASE pgpp_test;`
- Test user with full privileges on `pgpp_test`
- Environment: `PGPP_TEST_DBNAME`, `PGPP_TEST_HOST`, `PGPP_TEST_USER`, `PGPP_TEST_PASSWORD`, `PGPP_TEST_PORT`

### Connection Lifecycle

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-CONN-001 | `open` with valid connection string returns `true`, `isOpen` returns `true` | REQ-PGPP-010 | High |
| IT-CONN-002 | `open` with invalid connection string returns `false` | REQ-PGPP-010 | High |
| IT-CONN-003 | `isOpen` after successful `open` returns `true` | REQ-PGPP-010 | High |
| IT-CONN-004 | `close` sets `isOpen` to `false`, subsequent `close` is safe | REQ-PGPP-011 | High |
| IT-CONN-005 | `reset` restores a connection after simulated interruption | REQ-PGPP-042 | Medium |
| IT-CONN-006 | `lastError` returns non-empty message after a failed operation | REQ-PGPP-018 | Medium |

### Prepared Statements

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-STMT-001 | `prepare` succeeds with valid SQL and matching OIDs | REQ-PGPP-013 | High |
| IT-STMT-002 | `prepare` fails with syntactically invalid SQL | REQ-PGPP-013 | High |
| IT-STMT-003 | `isPrepared` returns `true` after successful `prepare` | REQ-PGPP-014 | High |
| IT-STMT-004 | `isPrepared` returns `false` for a name that was never prepared | REQ-PGPP-014 | High |

### Direct Execution (PgppConnection)

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-EXEC-001 | `execRaw` CREATE TABLE and DROP TABLE succeed | REQ-PGPP-015 | High |
| IT-EXEC-002 | `execPrepared` INSERT returns `true` | REQ-PGPP-016 | High |
| IT-EXEC-003 | `execPrepared` SELECT returns correct `std::string` results | REQ-PGPP-016, REQ-PGPP-017 | High |
| IT-EXEC-004 | `execPrepared` SELECT returns correct `int` results | REQ-PGPP-009 | High |
| IT-EXEC-005 | `execPrepared` SELECT returns correct `bool` results | REQ-PGPP-009 | High |
| IT-EXEC-006 | `execPrepared` SELECT returns correct `double` results | REQ-PGPP-009 | High |
| IT-EXEC-007 | `execPrepared` SELECT with NULL columns leaves default values | REQ-PGPP-008 | High |
| IT-EXEC-008 | `execPrepared` SELECT with multiple rows populates vector correctly | REQ-PGPP-017 | High |
| IT-EXEC-009 | `execPrepared` with wrong parameter count fails gracefully | REQ-PGPP-018 | Medium |

### Pool Operations

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-POOL-001 | Pool `initialize` with 2 connections succeeds, `totalConnections` returns 2 | REQ-PGPP-019 | High |
| IT-POOL-002 | Pool `execSync` INSERT returns `true` | REQ-PGPP-028 | High |
| IT-POOL-003 | Pool `querySync` SELECT returns correctly typed rows | REQ-PGPP-028 | High |
| IT-POOL-004 | Pool `execAsync` returns future that resolves to `true` | REQ-PGPP-029 | High |
| IT-POOL-005 | Pool `queryAsync` returns future with correctly typed rows | REQ-PGPP-029 | High |
| IT-POOL-006 | Pool `exec` callback fires on a worker thread (different thread ID) | REQ-PGPP-032 | Medium |
| IT-POOL-007 | Pool handles N concurrent queries from N threads without data corruption | REQ-PGPP-027, REQ-PGPP-044 | High |
| IT-POOL-008 | Pool statistics (`totalConnections`, `freeConnections`, `busyConnections`, `queuedRequests`) are accurate under load | REQ-PGPP-039, REQ-PGPP-040 | Medium |
| IT-POOL-009 | Pool `shutdown` with pending requests delivers `nullopt` via futures | REQ-PGPP-021 | High |
| IT-POOL-010 | Pool `prepareStatement` on running pool makes statement available to all workers | REQ-PGPP-026 | Medium |

### Transactions

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-TXN-001 | Transaction commits on success: inserted row is visible after commit | REQ-PGPP-036 | High |
| IT-TXN-002 | Transaction rolls back on exception: no row visible after rollback | REQ-PGPP-035 | High |
| IT-TXN-003 | Transaction returns `false` on BEGIN failure (e.g., connection issue) | REQ-PGPP-034 | Medium |

### Coroutines

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-CORO-001 | `coExec` executes a prepared INSERT successfully | REQ-PGPP-047 | Medium |
| IT-CORO-002 | `coQuery<RowTuple>` returns correctly typed rows | REQ-PGPP-049 | Medium |
| IT-CORO-003 | `FireAndForget` coroutine completes and self-destructs without leak | REQ-PGPP-045 | Medium |

### Raw SQL via Pool

| ID | Test | Requirement | Priority |
|---|---|---|---|
| IT-RAW-001 | `execRawSync` executes DDL (CREATE/DROP TABLE) | REQ-PGPP-015 | Medium |
| IT-RAW-002 | `execRawAsync` returns future that resolves to `true` for valid SQL | REQ-PGPP-029 | Medium |

---

## Implementation Plan

### Phase 1: Unit Tests (no database)

**Estimated effort:** 1-2 sessions

1. Set up test infrastructure (`tests/CMakeLists.txt`, test framework dependency)
2. Implement UT-CONV-* tests (type conversions are header-only, easy to test)
3. Implement UT-OID-001 (static assertions or value checks)
4. Implement UT-STMT-001
5. Implement UT-CONN-* tests (requires exposing `buildConnectionString` or `escapeConnValue`)
6. Implement UT-POOL-* tests (state machine edge cases)

**Blockers:**
- `buildConnectionString` and `escapeConnValue` are private/static. Options:
  - Add a `friend class PgppPoolTest;` declaration
  - Extract `escapeConnValue` to a utility header
  - Test indirectly via connection string output validation

### Phase 2: Integration Tests (with database)

**Estimated effort:** 2-3 sessions

1. Set up Docker/local PostgreSQL test environment configuration
2. Create test fixture: connects, creates temp tables in SetUp, drops in TearDown
3. Implement IT-CONN-* and IT-STMT-* (connection and statement tests)
4. Implement IT-EXEC-* (direct execution tests)
5. Implement IT-POOL-* (pool operation tests)
6. Implement IT-TXN-* (transaction tests)
7. Implement IT-CORO-* (coroutine tests)
8. Implement IT-RAW-* (raw SQL tests)

**Blockers:**
- PostgreSQL instance availability in CI
- Coroutine tests require a coroutine-capable test harness

### Phase 3: Stress and Edge Cases (future)

- Connection pool exhaustion under sustained load
- Behavior when PostgreSQL is restarted mid-operation
- Memory leak detection (valgrind/ASan)
- Thread sanitizer validation

---

## Test Infrastructure

### Directory Structure

```
tests/
  CMakeLists.txt          # Test target configuration
  unit/
    test_conversions.cpp  # UT-CONV-*
    test_connection_string.cpp  # UT-CONN-*
    test_pool_state.cpp   # UT-POOL-*
    test_structs.cpp      # UT-STMT-*, UT-OID-*
  integration/
    test_fixture.h        # Shared PostgreSQL setup/teardown
    test_connection.cpp   # IT-CONN-*
    test_statements.cpp   # IT-STMT-*
    test_execution.cpp    # IT-EXEC-*
    test_pool.cpp         # IT-POOL-*
    test_transactions.cpp # IT-TXN-*
    test_coroutines.cpp   # IT-CORO-*
    test_raw.cpp          # IT-RAW-*
```

### CMake Integration

```cmake
option(PGPP_BUILD_TESTS "Build pgpp tests" OFF)

if(PGPP_BUILD_TESTS)
    enable_testing()
    # Unit tests (always buildable)
    add_executable(pgpp_unit_tests ...)
    add_test(NAME pgpp_unit_tests COMMAND pgpp_unit_tests)

    # Integration tests (require PGPP_TEST_DBNAME env var)
    add_executable(pgpp_integration_tests ...)
    add_test(NAME pgpp_integration_tests COMMAND pgpp_integration_tests)
endif()
```
