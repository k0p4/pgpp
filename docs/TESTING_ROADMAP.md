# pgpp Testing Roadmap

## Overview

Testing strategy for the pgpp PostgreSQL connection pool library. Tests are divided into
unit tests (no database required) and integration tests (require a running PostgreSQL instance).

**Framework:** GoogleTest v1.14.0 (fetched via FetchContent)
**Test totals:** 66 unit + 112 integration = 178 test cases

---

## Unit Tests (no database needed)

Test pure logic: type conversions, connection string building, struct construction,
pool state machine basics. Located in `tests/unit/`, numeric-prefixed for ordering.

### test_0.0_basic.cpp — Basic Sanity

| ID | Test | Requirement |
|---|---|---|
| UT-BASIC-001 | `TypesAreDefaultConstructible` — PgppPool and PgppConnection default-construct | — |
| UT-BASIC-002 | `PoolIsDestructibleWithoutInit` — destroy pool without initialize | REQ-PGPP-023 |
| UT-BASIC-003 | `ConnectionIsDestructibleWithoutOpen` — destroy connection without open | REQ-PGPP-011 |

### test_1.0_structs.cpp — Structs & OID Constants

| ID | Test | Requirement |
|---|---|---|
| UT-STMT-001 | `StatementHoldsFields` — Statement struct holds name, SQL, variables | REQ-PGPP-004, REQ-PGPP-005 |
| UT-STMT-002 | `StatementMultipleParams` — Statement with multiple OID parameters | REQ-PGPP-005 |
| UT-STMT-003 | `PgppConnectionInfoDefaults` — default field values, including the pool/connection options | REQ-PGPP-001, REQ-PGPP-077, REQ-PGPP-078, REQ-PGPP-080 |
| UT-OID-001 | `MatchPostgreSQLCatalog` — pg:: OID constants match documented values | REQ-PGPP-006 |

### test_2.0_conversions.cpp — Binary Result Decoding

Tests `Internal::Details::decodeBinary<T>(oid, bytes)` on hand-built wire-format cells (network byte order).

| ID | Test | Requirement |
|---|---|---|
| UT-CONV-001 | `TextualColumnsIntoString` — text, varchar, bpchar, name, unknown, json, empty | REQ-PGPP-009 |
| UT-CONV-002 | `JsonbStripsVersionByte` — leading version byte removed; missing/unknown version is `Malformed` | REQ-PGPP-009 |
| UT-CONV-003 | `NonTextualColumnIntoStringIsTypeMismatch` — int4, timestamp, bool into std::string | REQ-PGPP-009 |
| UT-CONV-004 | `Int4Decodes` — 42, 0, -1, INT32_MAX, INT32_MIN | REQ-PGPP-009 |
| UT-CONV-005 | `Int2Decodes` | REQ-PGPP-009 |
| UT-CONV-006 | `Int8Decodes` — INT64_MAX / INT64_MIN | REQ-PGPP-009 |
| UT-CONV-007 | `NarrowerColumnWidens` — int2 into int / int64, int4 into int64 | REQ-PGPP-009 |
| UT-CONV-008 | `Int8IntoIntIsRangeChecked` — COUNT(*) into int; ±3000000000 is `OutOfRange` | REQ-PGPP-057 |
| UT-CONV-009 | `Int16RejectsOutOfRange` — ±40000 into int16_t is `OutOfRange` instead of wrapping | REQ-PGPP-057 |
| UT-CONV-010 | `Uint32FromOidAndIntegerColumns` — oid max ok; -1 and 5000000000 are `OutOfRange` | REQ-PGPP-057 |
| UT-CONV-011 | `TextIntoNumberIsTypeMismatch` | REQ-PGPP-009 |
| UT-CONV-012 | `NumericHasNoDecoder` — numeric into int64 / double is `TypeMismatch` | REQ-PGPP-009 |
| UT-CONV-013 | `WrongCellSizeIsMalformed` — 3-byte int4, 4-byte int8, 4-byte float8, 2-byte bool | REQ-PGPP-009 |
| UT-CONV-014 | `Float8Decodes` | REQ-PGPP-009 |
| UT-CONV-015 | `Float4Decodes` — into float and into double | REQ-PGPP-009 |
| UT-CONV-016 | `Float8IntoFloatIsRangeChecked` — ±1e39 is `OutOfRange` | REQ-PGPP-057 |
| UT-CONV-017 | `IntegerColumnIntoFloatingPoint` | REQ-PGPP-009 |
| UT-CONV-018 | `DoubleInfinityAndNaN` — pass through, also into float | REQ-PGPP-009 |
| UT-CONV-019 | `BoolDecodes` — 0/1; int4 or text into bool is `TypeMismatch` | REQ-PGPP-009 |
| UT-CONV-020 | `StringUtf8Multibyte` | REQ-PGPP-009 |
| UT-CONV-021 | `TextualTypeSet` — `isTextualType` over the pg:: catalog | REQ-PGPP-009 |
| UT-CONV-022 | `EmptyRowTupleInstantiates` — `std::vector<std::tuple<>>` result compiles (zero columns, no zero-length array) and fails cleanly on a closed connection | REQ-PGPP-063 |
| UT-CONV-023 | `OptionalWrapsTheInnerDecoder` — `std::optional<T>` decodes as `T`; every error passed through | REQ-PGPP-075 |
| UT-CONV-024 | `ParameterPointersAndEmbeddedNul` — `paramValue` (nullopt → nullptr), `paramHasEmbeddedNul` | REQ-PGPP-074, REQ-PGPP-075 |

### test_2.1_connection_string.cpp — Connection String Builder

Calls the public static `PgppPool::buildConnectionString`.

| ID | Test | Requirement |
|---|---|---|
| UT-CONN-001 | `BuildConnectionStringAllFields` — all fields produce valid libpq string | REQ-PGPP-001, REQ-PGPP-002 |
| UT-CONN-002 | `BuildConnectionStringEmptyDbname` — empty dbname returns empty string | REQ-PGPP-001 |
| UT-CONN-003 | `BuildConnectionStringEscapesSpecialChars` — escapes `'` and `\` | REQ-PGPP-002 |
| UT-CONN-004 | `BuildConnectionStringNoUser` — password omitted when no user | REQ-PGPP-003 |
| UT-CONN-005 | `BuildConnectionStringUserNoPassword` — user without password | REQ-PGPP-003 |
| UT-CONN-006 | `BuildConnectionStringPortZeroOmitted` — port=0 omitted from string | REQ-PGPP-001 |
| UT-CONN-007 | `BuildConnectionStringOptionsIncluded` | REQ-PGPP-001 |
| UT-CONN-008 | `BuildConnectionStringSslmodeIncluded` | REQ-PGPP-001 |
| UT-CONN-009 | `BuildConnectionStringMixedSpecialChars` | REQ-PGPP-002 |
| UT-CONN-010 | `BuildConnectionStringUsernameWithSpaces` | REQ-PGPP-002 |
| UT-CONN-011 | `BuildConnectionStringPortMax` — uint16 max | REQ-PGPP-001 |
| UT-CONN-012 | `BuildConnectionStringPortMin` — port=1 | REQ-PGPP-001 |
| UT-CONN-013 | `BuildConnectionStringMinimalFields` — dbname only | REQ-PGPP-001 |
| UT-CONN-014 | `BuildConnectionStringUnicodeDbname` | REQ-PGPP-002 |
| UT-CONN-015 | `BuildConnectionStringConnectTimeout` — `connect_timeout=N` | REQ-PGPP-077 |
| UT-CONN-016 | `BuildConnectionStringLibpqParamsAppendedLast` — verbatim, after the generated keywords | REQ-PGPP-077 |
| UT-CONN-017 | `BuildConnectionStringIsNoexceptStatic` | REQ-PGPP-061, REQ-PGPP-077 |

### test_3.0_pool_state.cpp — Pool State Machine

| ID | Test | Requirement |
|---|---|---|
| UT-POOL-001 | `DefaultPoolSize` — hardware_concurrency or 16 | REQ-PGPP-019 |
| UT-POOL-002 | `InitializeFailsWithBadConnection` | REQ-PGPP-019 |
| UT-POOL-003 | `DoubleShutdownSafe` | REQ-PGPP-020 |
| UT-POOL-004 | `UninitializedPoolOperations` — exec/query don't crash | REQ-PGPP-028, REQ-PGPP-029 |
| UT-POOL-005 | `ShutdownThenReinitialize` | REQ-PGPP-020, REQ-PGPP-019 |
| UT-POOL-006 | `EnqueueRawOnUninitializedPool` | REQ-PGPP-038 |
| UT-POOL-007 | `AsyncOnUninitializedPoolReturnsNullopt` | REQ-PGPP-029 |
| UT-POOL-008 | `PrepareStatementBeforeInitialize` — returns `true` (registered) | REQ-PGPP-025 |
| UT-POOL-009 | `PrepareStatementEmptyName` | REQ-PGPP-004 |
| UT-POOL-010 | `CallbackExecOnUninitializedPool` — `exec` and `query` fire once, with `nullopt`, on the calling thread, before returning | REQ-PGPP-033 |
| UT-POOL-011 | `TransactionOnUninitializedPool` — future resolved with `nullopt` (request refused) | REQ-PGPP-029, REQ-PGPP-038 |

### test_4.0_noexcept.cpp — The Library Never Throws

Injects throwing pieces from outside (a parameter type whose copy throws, a throwing callback,
a transaction functor whose copy throws) on an uninitialized pool; every call must return its
failure value instead of propagating.

| ID | Test | Requirement |
|---|---|---|
| UT-NOEXC-001 | `PublicApiIsNoexcept` — `static_assert(noexcept(...))` over the public API, `std::optional` parameters and columns included | REQ-PGPP-061 |
| UT-NOEXC-002 | `ExecAsyncArgumentCopyFailureYieldsNullopt` | REQ-PGPP-061, REQ-PGPP-029a |
| UT-NOEXC-003 | `QueryAsyncArgumentCopyFailureYieldsNullopt` | REQ-PGPP-061, REQ-PGPP-029a |
| UT-NOEXC-004 | `SyncArgumentCopyFailureReturnsFalse` — execSync / querySync | REQ-PGPP-061 |
| UT-NOEXC-005 | `CallbackFailuresAreContained` — fires once with nullopt; throwing callback contained | REQ-PGPP-061, REQ-PGPP-033 |
| UT-NOEXC-006 | `TransactionWorkCopyFailureYieldsNullopt` | REQ-PGPP-061 |
| UT-NOEXC-007 | `CoroutineArgumentCopyFailureCompletesWithNullopt` — coExec / coQuery | REQ-PGPP-061 |

---

## Integration Tests (require PostgreSQL)

Require a running PostgreSQL instance. The test executable auto-manages a Docker container
via `DockerPostgresEnvironment` (see `tests/common/docker_fixture.h`).
Located in `tests/integration/`, numeric-prefixed for ordering.

### Fixtures

| Fixture | Location | Description |
|---|---|---|
| `DockerPostgresEnvironment` | `common/docker_fixture.h` | Global environment: starts/stops PostgreSQL container |
| `PgppIntegrationTest` | `common/integration_fixture.h` | Pool with 2 connections, creates/drops test table |
| `PgppConnectionTest` | `common/integration_fixture.h` | Single direct connection, creates/drops test table |

### test_1.0_connection.cpp — Connection Lifecycle

| ID | Test | Requirement |
|---|---|---|
| IT-CONN-001 | `OpenValidConnection` | REQ-PGPP-010 |
| IT-CONN-002 | `OpenInvalidConnection` | REQ-PGPP-010 |
| IT-CONN-003 | `IsOpenAfterOpen` | REQ-PGPP-010 |
| IT-CONN-004 | `CloseAndDoubleClose` | REQ-PGPP-011 |
| IT-CONN-005 | `ResetRestoresConnection` | REQ-PGPP-042 |
| IT-CONN-006 | `LastErrorAfterFailure` | REQ-PGPP-018 |
| IT-CONN-007 | `ReconnectAfterClose` | REQ-PGPP-010 |
| IT-CONN-008 | `OpenTwiceWithoutClose` — idempotent open | REQ-PGPP-010 |
| IT-CONN-009 | `InvalidHostFastFail` | REQ-PGPP-010 |

### test_2.0_statements.cpp — Prepared Statements

Fixture: `PgppConnectionTest`

| ID | Test | Requirement |
|---|---|---|
| IT-STMT-001 | `PrepareValidSQL` | REQ-PGPP-013 |
| IT-STMT-002 | `PrepareInvalidSQL` | REQ-PGPP-013 |
| IT-STMT-003 | `IsPreparedAfterPrepare` | REQ-PGPP-014 |
| IT-STMT-004 | `IsPreparedFalseForUnknown` | REQ-PGPP-014 |
| IT-STMT-005 | `RePrepareAfterReset` | REQ-PGPP-043 |

### test_3.0_execution.cpp — Direct Execution (PgppConnection)

Fixture: `PgppConnectionTest`

| ID | Test | Requirement |
|---|---|---|
| IT-EXEC-001 | `ExecRawCreateDrop` | REQ-PGPP-015 |
| IT-EXEC-002 | `ExecPreparedInsert` | REQ-PGPP-016 |
| IT-EXEC-003 | `ExecPreparedSelectString` | REQ-PGPP-016, REQ-PGPP-017 |
| IT-EXEC-004 | `ExecPreparedSelectInt` | REQ-PGPP-009 |
| IT-EXEC-005 | `ExecPreparedSelectBool` | REQ-PGPP-009 |
| IT-EXEC-006 | `ExecPreparedSelectDouble` | REQ-PGPP-009 |
| IT-EXEC-007 | `ExecPreparedNullHandling` | REQ-PGPP-008 |
| IT-EXEC-008 | `ExecPreparedMultipleRows` | REQ-PGPP-017 |
| IT-EXEC-009 | `ExecPreparedWrongParamCount` | REQ-PGPP-018 |
| IT-EXEC-010 | `ExecPreparedAllNullRow` | REQ-PGPP-008 |
| IT-EXEC-011 | `ExecPreparedIntBoundaryValues` | REQ-PGPP-009 |
| IT-EXEC-012 | `ExecPreparedFloatPrecision` | REQ-PGPP-009 |
| IT-EXEC-013 | `ExecPreparedZeroRows` | REQ-PGPP-017 |
| IT-EXEC-014 | `ExecPreparedNoResultAcceptsRows` — `INSERT ... RETURNING` and `SELECT` through the no-result overload succeed; a real failure still fails | REQ-PGPP-073 |
| IT-EXEC-015 | `ExecPreparedOptionalColumnsAndParams` — NULL reads as `nullopt`, `nullopt` inserts NULL, plain columns keep the default, decoding errors still reported | REQ-PGPP-075, REQ-PGPP-008 |

### test_3.1_conversion_errors.cpp — Result Conversion Failures

Fixtures: `PgppConnectionTest`, `PgppIntegrationTest` (+ standalone single-worker test)

| ID | Test | Requirement |
|---|---|---|
| IT-CONVERR-001 | `ConversionOverflowFailsWithoutPartialRows` — no partial rows, existing rows kept | REQ-PGPP-057, REQ-PGPP-017 |
| IT-CONVERR-002 | `ConversionFailureEveryThrowingType` — int, int16 (text + out of range), int64, uint32 (text, negative, out of range), double, float | REQ-PGPP-057 |
| IT-CONVERR-003 | `ConversionFailureInLaterColumn` | REQ-PGPP-057 |
| IT-CONVERR-004 | `ConversionFailureQuerySync` — returns false, does not throw | REQ-PGPP-057 |
| IT-CONVERR-005 | `ConversionFailureQueryAsync` — future resolves with false, no broken promise | REQ-PGPP-057 |
| IT-CONVERR-006 | `ConversionFailureQueryCallback` — callback fires with false | REQ-PGPP-057 |
| IT-CONVERR-007 | `ConversionFailureCoQueryResumes` — coroutine resumed with false | REQ-PGPP-057, REQ-PGPP-058 |
| IT-CONVERR-008 | `SingleWorkerKeepsServing` (standalone) — repeated failures, then a good query | REQ-PGPP-041, REQ-PGPP-058 |

### test_4.0_pool.cpp — Pool Operations

Fixture: `PgppIntegrationTest` (+ standalone tests for shutdown scenarios)

| ID | Test | Requirement |
|---|---|---|
| IT-POOL-001 | `PoolInitializeWithConnections` | REQ-PGPP-019 |
| IT-POOL-002 | `DoubleInitializeReturnsTrueWhenAlreadyRunning` | REQ-PGPP-019 |
| IT-POOL-003 | `PoolExecSyncInsert` | REQ-PGPP-028 |
| IT-POOL-004 | `PoolQuerySyncSelect` | REQ-PGPP-028 |
| IT-POOL-005 | `PoolExecAsyncFuture` | REQ-PGPP-029 |
| IT-POOL-006 | `PoolQueryAsyncFuture` | REQ-PGPP-029 |
| IT-POOL-007 | `PoolCallbackOnWorkerThread` | REQ-PGPP-032 |
| IT-POOL-008 | `PoolConcurrentQueries` | REQ-PGPP-027, REQ-PGPP-044 |
| IT-POOL-009 | `PoolStatisticsIdle` | REQ-PGPP-039, REQ-PGPP-040 |
| IT-POOL-010 | `PoolStatisticsUnderLoad` — polls until both connections are leased | REQ-PGPP-039, REQ-PGPP-040 |
| IT-POOL-011 | `PoolPrepareOnRunningPool` — usable by the very next request, no waiting | REQ-PGPP-026 |
| IT-POOL-012 | `PoolQueryCallback` | REQ-PGPP-032 |
| IT-POOL-013 | `DuplicatePrepareStatementHandled` | REQ-PGPP-004 |
| IT-POOL-014 | `PoolExecZeroArgs` | REQ-PGPP-016 |
| IT-POOL-015 | `QueueSaturation` | REQ-PGPP-037, REQ-PGPP-040 |
| IT-POOL-016 | `WorkerRecoveryAfterBadQuery` | REQ-PGPP-041 |
| IT-POOL-017 | `PoolQueryReturningZeroRows` | REQ-PGPP-017 |
| IT-POOL-018 | `PendingRequestsGetNullopt` (standalone) — queued requests drain with `nullopt`, the running one is cancelled and reports `false`, `shutdown()` returns in under 2 s | REQ-PGPP-021, REQ-PGPP-076 |
| IT-POOL-019 | `SequentialQueries` (standalone, single connection) | REQ-PGPP-028 |
| IT-POOL-020 | `ShutdownDuringSlowQuery` (standalone) | REQ-PGPP-020 |
| IT-POOL-021 | `ShutdownCancelsInFlightStatement` / `ShutdownCancelsLeasedStatement` (standalone) — `pg_sleep(4)` on an executor thread and on a caller's lease; `shutdown()` returns in under 1.5 s, the statement reports `false` | REQ-PGPP-076 |
| IT-POOL-022 | `PoolExecOnSelectSucceeds` — `execSync` / `execAsync` / `exec` on `SELECT` and `INSERT ... RETURNING` | REQ-PGPP-073 |
| IT-POOL-023 | `ExcessRequestsAreRefusedAtOnce` (standalone) — `maxQueuedRequests = 2`, pool 1: the third waiting request resolves `nullopt` at once (future and callback); accepted ones complete | REQ-PGPP-078 |
| IT-POOL-024 | `DefaultPoolSizeIsOnePerCoreUpToEight` (standalone) | REQ-PGPP-079 |
| IT-POOL-025 | `PoolOptionalParamsAndColumns` — `std::optional` through `execSync` / `execAsync` / `querySync` / `queryAsync` | REQ-PGPP-075 |

### test_5.0_transactions.cpp — Transactions

Fixture: `PgppIntegrationTest`

| ID | Test | Requirement |
|---|---|---|
| IT-TXN-001 | `TransactionCommitsOnSuccess` | REQ-PGPP-036 |
| IT-TXN-002 | `TransactionRollsBackOnException` | REQ-PGPP-035 |
| IT-TXN-003 | `TransactionMultiStatement` | REQ-PGPP-036 |
| IT-TXN-004 | `TransactionConstraintViolationRollback` — failed INSERT inside `work` resolves `false`, original row intact | REQ-PGPP-065 |
| IT-TXN-005 | `TransactionEmptyCommits` | REQ-PGPP-036 |
| IT-TXN-006 | `TransactionManyStatements` | REQ-PGPP-036 |
| IT-TXN-007 | `TransactionDeadlockHandling` — exactly one side commits, the victim resolves `false`, rows reflect one transaction only | REQ-PGPP-065 |
| IT-TXN-008 | `TransactionFailedStatementRollsBack` — wallet example: CHECK violation on the first UPDATE, nothing applied, `false` | REQ-PGPP-065, REQ-PGPP-036 |
| IT-TXN-009 | `TransactionWorkReturnsBool` — `return false` rolls back and resolves `false`; `return true` commits | REQ-PGPP-066 |

### test_6.0_raw.cpp — Raw SQL via Pool

Fixture: `PgppIntegrationTest`

| ID | Test | Requirement |
|---|---|---|
| IT-RAW-001 | `ExecRawSyncDDL` | REQ-PGPP-015 |
| IT-RAW-002 | `ExecRawAsyncFuture` | REQ-PGPP-029 |
| IT-RAW-003 | `ExecRawSyncInvalidSQL` | REQ-PGPP-015 |
| IT-RAW-004 | `ExecRawSyncSelect` | REQ-PGPP-015 |
| IT-RAW-005 | `ExecRawMultiStatement` | REQ-PGPP-015 |
| IT-RAW-006 | `ExecRawDDLSequence` | REQ-PGPP-015 |
| IT-RAW-007 | `ExecRawPartialFailure` | REQ-PGPP-018 |

### test_7.0_coroutines.cpp — Coroutines

Fixture: `PgppIntegrationTest` (+ standalone shutdown test)

| ID | Test | Requirement |
|---|---|---|
| IT-CORO-001 | `CoExecInsert` | REQ-PGPP-047 |
| IT-CORO-002 | `CoQueryReturnsRows` | REQ-PGPP-049 |
| IT-CORO-003 | `FireAndForgetSelfDestructs` | REQ-PGPP-045 |
| IT-CORO-004 | `CoExecPreparedBackwardCompat` | REQ-PGPP-051 |
| IT-CORO-005 | `CoExecFailingQuery` | REQ-PGPP-047 |
| IT-CORO-006 | `CoAwaitOnShutdownPool` (standalone) | REQ-PGPP-029 |

### test_8.0_stress.cpp — Stress & Edge Cases

Fixture: `PgppIntegrationTest` (+ standalone pool tests)

| ID | Test | Requirement |
|---|---|---|
| IT-STRESS-001 | `ExhaustionUnderSustainedLoad` (standalone) | REQ-PGPP-037, REQ-PGPP-044 |
| IT-STRESS-002 | `ConnectionSurvivesBackendKill` (standalone) — reset mid-transaction; the next raw and prepared requests succeed (statements prepared again) | REQ-PGPP-042, REQ-PGPP-043 |
| IT-STRESS-003 | `CallbackInFlightAtShutdown` (standalone) — every callback fires once before `shutdown()` returns; served ones succeed except the one cancelled by shutdown | REQ-PGPP-021, REQ-PGPP-033, REQ-PGPP-070, REQ-PGPP-076 |
| IT-STRESS-004 | `ConcurrentPrepareStatementFromThreads` | REQ-PGPP-026, REQ-PGPP-027 |
| IT-STRESS-005 | `CoroutineThrowAfterAwait` | REQ-PGPP-046 |
| IT-STRESS-006 | `ShutdownWhileSuspended` (standalone) | REQ-PGPP-021, REQ-PGPP-022 |
| IT-STRESS-007 | `ParamLongString` | REQ-PGPP-016 |
| IT-STRESS-008 | `ParamUnicode` | REQ-PGPP-016 |
| IT-STRESS-009 | `ParamEmptyString` | REQ-PGPP-016 |
| IT-STRESS-010 | `ParamEmbeddedNulIsRejected` — refused by both overloads, nothing inserted | REQ-PGPP-074 |
| IT-STRESS-011 | `PrepareStatementRightAfterInitialize` (standalone) — statement registered right after `initialize()` is usable at once | REQ-PGPP-059, REQ-PGPP-026 |
| IT-STRESS-012 | `ShutdownRightAfterInitializeDoesNotHang` (standalone) — many initialize/shutdown cycles under a watchdog | REQ-PGPP-060, REQ-PGPP-020 |

### test_9.0_reentrancy.cpp — Calling the Pool from Its Own Threads; Lifecycle Races; Connection Hygiene

All standalone (`TEST`, own pools). Tests that can hang run under a watchdog; a deadlock is a
failed assertion. IT-REENTRY-007..009 are data-race tests whose detector is ThreadSanitizer.

| ID | Test | Requirement |
|---|---|---|
| IT-REENTRY-001 | `SyncCallFromCallbackRunsInline` — pool 1, `execRawSync` inside an `exec` callback returns `true` (the callback's connection was returned before the callback ran) | REQ-PGPP-068, REQ-PGPP-069 |
| IT-REENTRY-002 | `SyncPathDoesNotUseExecutor` — both executor threads held by sleeping callbacks; a sync call completes at once | REQ-PGPP-068 |
| IT-REENTRY-003 | `PoolCallInsideWorkIsIndependent` — pool 2, a pool call inside `work` runs on its own connection, autocommitted, independent of the rollback | REQ-PGPP-067 |
| IT-REENTRY-004 | `TransactionFromCallbackCommits` — pool 1, a transaction run from inside a callback | REQ-PGPP-068 |
| IT-REENTRY-005 | `NestedTransactionIsIndependent` — inner transaction on its own connection commits; outer rollback does not undo it | REQ-PGPP-067 |
| IT-REENTRY-006 | `CallbackIssuedFromCallbackRunsLater` — pool 1, inner callback runs after the outer `exec` returned | REQ-PGPP-069 |
| IT-REENTRY-007 | `ShutdownFromCallbackCompletesTeardown` — 50 cycles; pending requests resolve; later `shutdown()` and destructor join the exiting threads | REQ-PGPP-070 |
| IT-REENTRY-008 | `StatsDuringShutdownAreRaceFree` — stats getters looping across initialize/shutdown cycles (TSan) | REQ-PGPP-071 |
| IT-REENTRY-009 | `ConcurrentInitializeShutdown` — `initialize` and `shutdown` from two threads; end state fully up or fully down (TSan) | REQ-PGPP-071 |
| IT-REENTRY-010 | `MixedApiStorm` — 6 clients × every API, callbacks that use the sync API and fire callbacks, `shutdown()` mid-storm, pool sizes 1 / 2 / 8; every request completes exactly once | REQ-PGPP-067..070 |
| IT-REENTRY-011 | `ReleaseRollsBackStrayTransaction` — pool 1: an INSERT after another caller's stray `BEGIN` persists; an aborted transaction does not poison the next caller | REQ-PGPP-072 |

### test_9.1_session.cpp — Session State Between Callers

All standalone.

| ID | Test | Requirement |
|---|---|---|
| IT-SESSION-001 | `SetLeaksBetweenCallersByDefault` — pool 4, 8 clients × 250 requests, one `SET`: later requests see it (pins the documented default) | REQ-PGPP-072 |
| IT-SESSION-002 | `SetDoesNotLeakWithSessionReset` — same load with `resetSessionAfterRequest`: 0 leaks | REQ-PGPP-080 |
| IT-SESSION-003 | `TempStateDoesNotLeakWithSessionReset` — pool 1: temp table gone, advisory lock released (a second session can take it), `LISTEN` undone, prepared statements kept | REQ-PGPP-080 |
| IT-SESSION-004 | `SetLocalInsideTransactionDoesNotLeak` — the zero-cost pattern | REQ-PGPP-072 |

---

## Future Work

- Behavior when PostgreSQL is restarted mid-operation

---

## Test Infrastructure

### Directory Structure

```
tests/
  CMakeLists.txt              # GLOB-based test discovery
  common/
    test_config.h             # Test configuration constants, getTestConnectionInfo()
    docker_fixture.h          # DockerPostgresEnvironment (auto-manage PostgreSQL container)
    integration_fixture.h     # PgppIntegrationTest, PgppConnectionTest fixtures
  unit/
    test_0.0_basic.cpp        # UT-BASIC-*
    test_1.0_structs.cpp      # UT-STMT-*, UT-OID-*
    test_2.0_conversions.cpp  # UT-CONV-*
    test_2.1_connection_string.cpp  # UT-CONN-*
    test_3.0_pool_state.cpp   # UT-POOL-*
    test_4.0_noexcept.cpp     # UT-NOEXC-*
  integration/
    main.cpp                  # Custom main() with DockerPostgresEnvironment registration
    test_1.0_connection.cpp   # IT-CONN-*
    test_2.0_statements.cpp   # IT-STMT-*
    test_3.0_execution.cpp    # IT-EXEC-*
    test_3.1_conversion_errors.cpp  # IT-CONVERR-*
    test_4.0_pool.cpp         # IT-POOL-*
    test_5.0_transactions.cpp # IT-TXN-*
    test_6.0_raw.cpp          # IT-RAW-*
    test_7.0_coroutines.cpp   # IT-CORO-*
    test_8.0_stress.cpp       # IT-STRESS-*
    test_9.0_reentrancy.cpp   # IT-REENTRY-*
    test_9.1_session.cpp      # IT-SESSION-*
```

### CMake Integration

```cmake
option(PGPP_BUILD_TESTS "Build pgpp tests" OFF)

if(PGPP_BUILD_TESTS)
    enable_testing()

    # Unit tests — GLOB discovers all .cpp in unit/
    file(GLOB_RECURSE UNIT_TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)
    add_executable(pgpp_unit_tests ${UNIT_TEST_SOURCES})
    target_link_libraries(pgpp_unit_tests PRIVATE pgpp GTest::gtest_main)
    add_test(NAME pgpp_unit_tests COMMAND pgpp_unit_tests)

    # Integration tests — GLOB discovers all .cpp in integration/
    # Uses custom main() (not gtest_main) for Docker environment setup
    file(GLOB_RECURSE INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS integration/*.cpp)
    add_executable(pgpp_integration_tests ${INTEGRATION_TEST_SOURCES})
    target_include_directories(pgpp_integration_tests PRIVATE common/)
    target_link_libraries(pgpp_integration_tests PRIVATE pgpp GTest::gtest)
    add_test(NAME pgpp_integration_tests COMMAND pgpp_integration_tests)
endif()
```

### Docker Fixture

Integration tests auto-manage a PostgreSQL container:
- **Image:** `postgres:16-alpine` (`PGPP_TEST_PG_IMAGE` overrides; CI runs 13, 14, 15, 17 too)
- **Container:** `pgpp-test-pg-7f3a`
- **Port:** 127.0.0.1:15432 (loopback only: the container's superuser has a well-known password)
- **Skip:** Set `PGPP_SKIP_DOCKER=1` to use an external PostgreSQL instance

### CI

| Job | What it adds |
|---|---|
| Build-Linux (gcc / clang × Debug / Release) | warnings as errors, unit + integration (Docker) |
| ThreadSanitizer | the whole suite under TSan |
| AddressSanitizer | the whole suite under ASan + UBSan |
| PostgreSQL-Versions (13, 14, 15, 17) | the integration suite against each supported server |
| Build-Windows / Build-MacOS | unit + integration against a PostgreSQL set up by `ikalnytskyi/action-setup-postgres` |
| CodeChecker | clang-tidy + clang static analyzer over the library and the tests |

### Test Naming Convention

Files use numeric prefixes (`N.M_topic.cpp`) to control execution order and logical grouping.
The major number groups related areas, the minor number orders within a group.
