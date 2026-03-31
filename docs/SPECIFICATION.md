# pgpp Specification

> C++20 PostgreSQL Connection Pool Library

## Overview

pgpp is a thread-safe PostgreSQL connection pool library built on libpq. It provides
synchronous, future-based, callback-based, and coroutine-based interfaces for executing
prepared statements and raw SQL. Each pool connection runs on a dedicated worker thread
with automatic reconnection on connection loss.

**License:** GPL-3.0-or-later

---

## 1. Data Structures

### 1.1 PgppConnectionInfo

Configuration struct for database connection parameters.

| Field | Type | Default | Description |
|---|---|---|---|
| `dbname` | `std::string` | `""` | Database name (required) |
| `host` | `std::string` | `""` | Server hostname |
| `sslmode` | `std::string` | `""` | SSL mode (disable, allow, prefer, require, etc.) |
| `options` | `std::string` | `""` | Additional libpq options |
| `user` | `std::string` | `""` | Database user |
| `password` | `std::string` | `""` | User password |
| `port` | `uint16_t` | `5432` | Server port |

**Requirements:**

- **REQ-PGPP-001:** `dbname` must be non-empty; `buildConnectionString` returns empty string if `dbname` is empty.
- **REQ-PGPP-002:** Special characters in field values (`'` and `\`) must be escaped in the connection string using backslash escaping within single-quoted values.
- **REQ-PGPP-003:** `password` is only included in the connection string if `user` is also non-empty.

### 1.2 Statement

Prepared statement descriptor.

| Field | Type | Description |
|---|---|---|
| `statementName` | `std::string` | Unique name for the prepared statement |
| `statement` | `std::string` | SQL text with `$1`, `$2`, ... parameter placeholders |
| `variables` | `std::vector<uint32_t>` | PostgreSQL OIDs for each parameter |

**Requirements:**

- **REQ-PGPP-004:** `statementName` must be unique per connection. Preparing a duplicate name on the same connection will fail.
- **REQ-PGPP-005:** The number of entries in `variables` must match the number of `$N` placeholders in `statement`.

### 1.3 PgppRequest

Internal request envelope for the worker queue.

| Field | Type | Description |
|---|---|---|
| `task` | `std::function<void(PgppConnection*)>` | Work function executed on a worker thread. Receives `nullptr` if the connection is unavailable. |

---

## 2. pg:: Namespace OID Constants

Type OID constants matching the PostgreSQL system catalog (`pg_type.oid`). Used in `Statement::variables`.

| Constant | Value | PostgreSQL Type |
|---|---|---|
| `pg::BYTEA` | 17 | bytea |
| `pg::CHAR` | 18 | "char" |
| `pg::INT8` | 20 | bigint (int8) |
| `pg::INT2` | 21 | smallint (int2) |
| `pg::INT4` | 23 | integer (int4) |
| `pg::TEXT` | 25 | text |
| `pg::FLOAT4` | 700 | real (float4) |
| `pg::FLOAT8` | 701 | double precision (float8) |
| `pg::VARCHAR` | 1043 | varchar |
| `pg::DATE` | 1082 | date |
| `pg::TIME` | 1083 | time |
| `pg::TIMESTAMP` | 1114 | timestamp |
| `pg::TIMESTAMPTZ` | 1184 | timestamptz |
| `pg::TIMETZ` | 1266 | timetz |

Legacy OID macros (e.g., `TEXTOID`, `INT4OID`) are available only when `PGPP_ENABLE_OID_MACROS` is defined before including the header.

**Requirements:**

- **REQ-PGPP-006:** OID values must match the PostgreSQL system catalog exactly.
- **REQ-PGPP-007:** Legacy macros must delegate to `pg::` namespace constants (not hardcoded duplicates).

---

## 3. Type Conversions

The `Internal::Details::convertPQValue<T>` template family converts libpq text-format results to C++ types.

| C++ Type | Conversion | NULL Handling |
|---|---|---|
| `std::string` | Raw string passthrough | Default-constructed (empty string) |
| `int` | `std::stoi` | Default-constructed (0) |
| `int16_t` | `std::stoi` then `static_cast<int16_t>` | Default-constructed (0) |
| `int64_t` | `std::stoll` | Default-constructed (0) |
| `uint32_t` | `std::stoul` then `static_cast<uint32_t>` | Default-constructed (0) |
| `double` | `std::stod` | Default-constructed (0.0) |
| `float` | `std::stof` | Default-constructed (0.0f) |
| `bool` | `true` if first char is `t`, `T`, or `1`; `false` otherwise | Default-constructed (false) |

**Requirements:**

- **REQ-PGPP-008:** NULL values (detected via `PQgetisnull`) must leave the target at its default-constructed value; the converter is never called for NULL fields.
- **REQ-PGPP-009:** Conversion functions must handle all valid PostgreSQL text representations for their respective types.

---

## 4. PgppConnection

Single-connection wrapper around a `PGconn*`. NOT thread-safe.

### 4.1 Lifecycle

| Method | Signature | Description |
|---|---|---|
| Constructor | `PgppConnection()` | Initializes `m_connection` to `nullptr` |
| Destructor | `~PgppConnection()` | Calls `close()` |
| `open` | `bool open(const std::string& connectionInfo)` | Connects via `PQconnectdb`. Returns `true` on success. If already open, returns `true` immediately. On failure, calls `PQfinish` and sets `m_connection` to `nullptr`. |
| `isOpen` | `bool isOpen() const` | Returns `true` if `m_connection != nullptr && PQstatus == CONNECTION_OK` |
| `reset` | `void reset()` | Calls `PQreset` to re-establish a lost connection |
| `close` | `void close()` | Calls `PQfinish` and sets `m_connection` to `nullptr` |
| `lastError` | `std::string lastError() const` | Returns `PQerrorMessage` or empty string if no connection |
| `connection` | `PGconn* connection()` | Returns raw `PGconn*` for direct libpq calls |

**Requirements:**

- **REQ-PGPP-010:** `open` must be idempotent -- calling it on an already-open connection returns `true` without reconnecting.
- **REQ-PGPP-011:** `close` must be safe to call multiple times (guards on `m_connection != nullptr`).
- **REQ-PGPP-012:** Copy construction and copy assignment are deleted.

### 4.2 Prepared Statements

| Method | Signature | Description |
|---|---|---|
| `prepare` | `bool prepare(const Statement& stmt)` | Calls `PQprepare` with the statement's OIDs. Returns `false` if connection is closed or preparation fails. |
| `isPrepared` | `bool isPrepared(const std::string& name)` | Uses `PQdescribePrepared` to check if a statement exists on this connection. |

**Requirements:**

- **REQ-PGPP-013:** `prepare` must fail gracefully if the connection is not open (returns `false`, logs error).
- **REQ-PGPP-014:** `isPrepared` must return `false` if the connection is not open.

### 4.3 Query Execution

| Method | Signature | Description |
|---|---|---|
| `execRaw` | `bool execRaw(const std::string& sql)` | Executes raw SQL via `PQexec`. Returns `true` if status is `PGRES_COMMAND_OK` or `PGRES_TUPLES_OK`. |
| `execPrepared` (no results) | `template<typename... Ts> bool execPrepared(name, args...)` | Executes a prepared statement expecting `PGRES_COMMAND_OK` (INSERT, UPDATE, DELETE). All args must have `.c_str()`. |
| `execPrepared` (with results) | `template<typename... Ts, typename... TAs> bool execPrepared(name, vector<tuple<TAs...>>&, args...)` | Executes a prepared statement expecting `PGRES_TUPLES_OK` (SELECT). Appends rows to the result vector. |

**Requirements:**

- **REQ-PGPP-015:** `execRaw` must return `false` if the connection is not open.
- **REQ-PGPP-016:** `execPrepared` passes all arguments as text-format strings via `.c_str()`.
- **REQ-PGPP-017:** The result overload of `execPrepared` must append to (not replace) the result vector, using `reserve` for efficiency.
- **REQ-PGPP-018:** Failed executions must log the error via `logTemplateError`.

---

## 5. PgppPool

Thread-safe connection pool. One worker thread per connection.

### 5.1 Lifecycle

| Method | Signature | Description |
|---|---|---|
| Constructor | `PgppPool()` | Sets `m_poolSize` to `hardware_concurrency()` (falls back to 16 if 0) |
| Destructor | `~PgppPool()` | Calls `shutdown()` |
| `initialize` | `bool initialize(const PgppConnectionInfo&, size_t poolSize=0)` | Builds connection string, creates connections, prepares registered statements, starts worker threads. If `poolSize > 0`, overrides the default. |
| `shutdown` | `void shutdown()` | Stops workers, drains pending requests (delivers `nullptr`), closes connections |
| `isInitialized` | `bool isInitialized() const` | Atomic check |

**Requirements:**

- **REQ-PGPP-019:** `initialize` must be idempotent -- second call returns `true` without reinitializing.
- **REQ-PGPP-020:** `shutdown` must be safe to call multiple times (atomic guard on `m_initialized`).
- **REQ-PGPP-021:** `shutdown` must drain all pending requests, invoking each task with `nullptr` so that futures receive `std::nullopt`.
- **REQ-PGPP-022:** Pending requests are drained outside the queue lock to avoid deadlock when tasks call `enqueueRaw` (e.g., coroutine resumption).
- **REQ-PGPP-023:** Destructor must call `shutdown()`.
- **REQ-PGPP-024:** Copy construction and copy assignment are deleted.

### 5.2 Statement Management

| Method | Signature | Description |
|---|---|---|
| `prepareStatement` | `void prepareStatement(const Statement&)` | Registers a statement. If workers are running, increments `m_stmtVersion` and wakes all workers to re-prepare. If workers are not running (pre-initialize), prepares on existing connections directly. |

**Requirements:**

- **REQ-PGPP-025:** Statements registered before `initialize` are prepared during connection creation.
- **REQ-PGPP-026:** Statements registered after `initialize` trigger lazy re-preparation on all worker threads via the `m_stmtVersion` atomic.
- **REQ-PGPP-027:** Statement list is protected by `m_stmtMutex`.

### 5.3 Synchronous API

| Method | Signature | Description |
|---|---|---|
| `execSync` | `template<Ts...> bool execSync(name, args...)` | Blocks on `execAsync(...).get()`. Returns `true` only if the future contains `true`. |
| `querySync` | `template<RowTuple, Ts...> pair<bool, vector<RowTuple>> querySync(name, args...)` | Blocks on `queryAsync(...).get()`. Returns `{false, {}}` if the future contained `nullopt`. |
| `execRawSync` | `bool execRawSync(const string& sql)` | Blocks on `execRawAsync(...).get()`. |

**Requirements:**

- **REQ-PGPP-028:** Synchronous methods must block on the corresponding async future -- they must not bypass the worker queue.

### 5.4 Future-Based API

| Method | Signature | Return |
|---|---|---|
| `execAsync` | `template<Ts...> future<optional<bool>> execAsync(name, args...)` | `nullopt` on shutdown/null conn; `true`/`false` on success/failure |
| `queryAsync` | `template<RowTuple, Ts...> future<pair<optional<bool>, vector<RowTuple>>> queryAsync(name, args...)` | `{nullopt, {}}` on shutdown/null conn |
| `execRawAsync` | `future<optional<bool>> execRawAsync(const string& sql)` | `nullopt` on shutdown/null conn |

**Requirements:**

- **REQ-PGPP-029:** If `m_shuttingDown` is `true` at call time, the future must be immediately resolved with `nullopt`.
- **REQ-PGPP-030:** If `enqueueRaw` fails (returns `false`), the promise must be resolved with `nullopt`.
- **REQ-PGPP-031:** If the worker receives a `nullptr` connection, the promise must be resolved with `nullopt`.

### 5.5 Callback-Based API

| Method | Signature | Description |
|---|---|---|
| `exec` | `template<Ts...> void exec(name, callback, args...)` | Callback fires on a worker thread with `optional<bool>` |
| `query` | `template<RowTuple, Ts...> void query(name, callback, args...)` | Callback fires on a worker thread with `optional<bool>` and `vector<RowTuple>` |

**Requirements:**

- **REQ-PGPP-032:** Callbacks execute on the worker thread, not the calling thread.
- **REQ-PGPP-033:** If shutting down or enqueue fails, the callback receives `nullopt` immediately on the calling thread.

### 5.6 Transactions

| Method | Signature | Description |
|---|---|---|
| `transaction` | `template<F> future<optional<bool>> transaction(F&& work)` | Executes `BEGIN`, calls `work(PgppConnection&)`, then `COMMIT`. On exception, executes `ROLLBACK` and resolves with `false`. |

**Requirements:**

- **REQ-PGPP-034:** If `BEGIN` fails, resolve with `false` without calling the work function.
- **REQ-PGPP-035:** Any exception thrown by `work` must trigger `ROLLBACK` and resolve with `false`.
- **REQ-PGPP-036:** On successful `COMMIT`, resolve with the result of `PQresultStatus == PGRES_COMMAND_OK`.

### 5.7 Raw Enqueue

| Method | Signature | Description |
|---|---|---|
| `enqueueRaw` | `bool enqueueRaw(unique_ptr<PgppRequest>)` | Pushes a request onto the queue. Returns `false` if shutting down. Public for coroutine awaitables. |

**Requirements:**

- **REQ-PGPP-037:** Must hold `m_queueMutex` during the push and `notify_one`.
- **REQ-PGPP-038:** Must return `false` (without enqueuing) if `m_shuttingDown` is `true`.

### 5.8 Pool Statistics

| Method | Return | Description |
|---|---|---|
| `totalConnections()` | `size_t` | Number of connections created |
| `freeConnections()` | `size_t` | `total - busy` (clamped to 0) |
| `busyConnections()` | `size_t` | Current value of `m_busyWorkers` atomic |
| `queuedRequests()` | `size_t` | Current queue size (acquires lock) |

**Requirements:**

- **REQ-PGPP-039:** `freeConnections` must never underflow (returns 0 if `busy > total`).
- **REQ-PGPP-040:** `queuedRequests` must acquire `m_queueMutex` for a consistent read.

---

## 6. Worker Thread Model

### 6.1 Thread Architecture

- One `std::thread` per connection (`m_workerThreads[i]` owns `m_connections[i]`).
- Workers block on `m_requestQueued` condition variable, waking on: new request, shutdown, or statement version change.

### 6.2 Worker Loop (`workerLoop`)

```
while (!shuttingDown):
    lock(queueMutex)
    wait(requestQueued, predicate: !queue.empty || shuttingDown || stmtVersion changed)
    if shuttingDown: break
    if queue not empty: pop request
    unlock

    if stmtVersion changed: re-prepare all statements on this connection

    if no request: continue

    if connection lost:
        PQreset
        if restored: re-prepare statements
        else: task(nullptr), continue

    busyWorkers++
    try: task(conn)
    catch: log exception
    busyWorkers--
```

**Requirements:**

- **REQ-PGPP-041:** Workers must catch all exceptions from tasks (both `std::exception` and `...`).
- **REQ-PGPP-042:** On connection loss, the worker must attempt `PQreset`. If reset fails, deliver `nullptr` to the task.
- **REQ-PGPP-043:** After successful reconnection, all registered statements must be re-prepared on that connection.
- **REQ-PGPP-044:** `m_busyWorkers` must be incremented before task execution and decremented after, using `memory_order_relaxed`.

---

## 7. Coroutines (pgpp_coroutines.h)

C++20 coroutine integration for `PgppPool`.

### 7.1 FireAndForget

Minimal coroutine return type for fire-and-forget async tasks.

- `initial_suspend` returns `suspend_never` (starts immediately).
- `final_suspend` returns `suspend_never` (self-destructs on completion).
- `unhandled_exception` catches and logs to `stderr`.

**Requirements:**

- **REQ-PGPP-045:** Must never leak the coroutine frame -- `final_suspend` must return `suspend_never`.
- **REQ-PGPP-046:** Unhandled exceptions must be caught and logged, never propagated.

### 7.2 DbExecAwaitable<Ts...>

Awaitable for executing a prepared statement without results.

- `await_ready` returns `false` (always suspends).
- `await_suspend` creates a `PgppRequest`, enqueues it, resumes the coroutine handle from the worker thread.
- `await_resume` returns `std::optional<bool>` (`nullopt` if connection was `nullptr`).

**Requirements:**

- **REQ-PGPP-047:** The coroutine resumes on the worker thread (not the original thread).
- **REQ-PGPP-048:** Arguments are captured by value in a `std::tuple` to ensure lifetime safety.

### 7.3 DbResultAwaitable<RowTuple, Ts...>

Awaitable for executing a prepared statement with results.

- `await_resume` returns `std::pair<std::optional<bool>, std::vector<RowTuple>>`.

**Requirements:**

- **REQ-PGPP-049:** Result rows are stored in a member `m_rows` and moved out in `await_resume`.

### 7.4 Factory Functions

| Function | Returns |
|---|---|
| `coExec(db, name, args...)` | `DbExecAwaitable<decay_t<Ts>...>` |
| `coQuery<RowTuple>(db, name, args...)` | `DbResultAwaitable<RowTuple, decay_t<Ts>...>` |
| `coExecPrepared(...)` | Backward-compat alias for `coExec` |
| `coExecPreparedWithResult<RowTuple>(...)` | Backward-compat alias for `coQuery` |

**Requirements:**

- **REQ-PGPP-050:** Factory functions must use `std::decay_t` on argument types to strip references and cv-qualifiers.
- **REQ-PGPP-051:** Backward-compatibility aliases must forward all arguments identically to the primary functions.

---

## 8. Logging

Compile-time selectable logging via `src/pgpp_log.h`.

| Define | Behavior |
|---|---|
| `PGPP_USE_ALOG` | Uses `alog` library (set automatically when `alog` CMake target exists) |
| `PGPP_USE_STDERR` | Simple `stderr` output |
| Neither | No-op (zero overhead) |

Macros: `PGPP_LOGV`, `PGPP_LOGD`, `PGPP_LOGW`, `PGPP_LOGE`.

**Requirements:**

- **REQ-PGPP-052:** When no logging define is set, log macros must compile to zero-cost no-ops.
- **REQ-PGPP-053:** `PGPP_USE_ALOG` is automatically defined by CMake when the `alog` target is available.

---

## 9. Build System

- CMake 3.16+, C++20 required.
- Static library target: `pgpp`.
- Public dependency: `PostgreSQL::PostgreSQL` (via `find_package`).
- Optional private dependency: `alog` (auto-detected via CMake target existence).
- Public include directory: `include/`.
- Header files: `include/pgpp/pgpp.h`, `include/pgpp/pgpp_connection.h`, `include/pgpp/pgpp_coroutines.h`.
- Consumed via CMake `FetchContent` by downstream projects.

**Requirements:**

- **REQ-PGPP-054:** The library must build without `alog` present (logging becomes no-op).
- **REQ-PGPP-055:** PostgreSQL client library (libpq) must be found via `find_package(PostgreSQL REQUIRED)`.
- **REQ-PGPP-056:** The library must support PostgreSQL versions 13 through 17.

---

## Requirements Index

| ID | Summary | Section |
|---|---|---|
| REQ-PGPP-001 | dbname required for connection string | 1.1 |
| REQ-PGPP-002 | Special char escaping in connection string | 1.1 |
| REQ-PGPP-003 | Password requires user | 1.1 |
| REQ-PGPP-004 | Unique statement names per connection | 1.2 |
| REQ-PGPP-005 | Variable count matches placeholders | 1.2 |
| REQ-PGPP-006 | OID values match PostgreSQL catalog | 2 |
| REQ-PGPP-007 | Legacy macros delegate to pg:: constants | 2 |
| REQ-PGPP-008 | NULL leaves default value | 3 |
| REQ-PGPP-009 | Converters handle valid PG text formats | 3 |
| REQ-PGPP-010 | open is idempotent | 4.1 |
| REQ-PGPP-011 | close is safe to call multiple times | 4.1 |
| REQ-PGPP-012 | Non-copyable connection | 4.1 |
| REQ-PGPP-013 | prepare fails gracefully if not open | 4.2 |
| REQ-PGPP-014 | isPrepared returns false if not open | 4.2 |
| REQ-PGPP-015 | execRaw fails if not open | 4.3 |
| REQ-PGPP-016 | Parameters passed as text via .c_str() | 4.3 |
| REQ-PGPP-017 | Result vector appended, not replaced | 4.3 |
| REQ-PGPP-018 | Failed exec logs error | 4.3 |
| REQ-PGPP-019 | Pool initialize is idempotent | 5.1 |
| REQ-PGPP-020 | Pool shutdown is safe to call multiple times | 5.1 |
| REQ-PGPP-021 | Shutdown drains pending requests with nullptr | 5.1 |
| REQ-PGPP-022 | Drain outside lock to avoid deadlock | 5.1 |
| REQ-PGPP-023 | Destructor calls shutdown | 5.1 |
| REQ-PGPP-024 | Non-copyable pool | 5.1 |
| REQ-PGPP-025 | Pre-init statements prepared during creation | 5.2 |
| REQ-PGPP-026 | Post-init statements trigger lazy re-prepare | 5.2 |
| REQ-PGPP-027 | Statement list mutex-protected | 5.2 |
| REQ-PGPP-028 | Sync methods block on async futures | 5.3 |
| REQ-PGPP-029 | Shutting down resolves future with nullopt | 5.4 |
| REQ-PGPP-030 | Enqueue failure resolves with nullopt | 5.4 |
| REQ-PGPP-031 | Null connection resolves with nullopt | 5.4 |
| REQ-PGPP-032 | Callbacks execute on worker thread | 5.5 |
| REQ-PGPP-033 | Shutdown/enqueue fail delivers nullopt to callback | 5.5 |
| REQ-PGPP-034 | BEGIN failure resolves with false | 5.6 |
| REQ-PGPP-035 | Exception triggers ROLLBACK | 5.6 |
| REQ-PGPP-036 | COMMIT result determines future value | 5.6 |
| REQ-PGPP-037 | enqueueRaw holds mutex during push | 5.7 |
| REQ-PGPP-038 | enqueueRaw returns false if shutting down | 5.7 |
| REQ-PGPP-039 | freeConnections never underflows | 5.8 |
| REQ-PGPP-040 | queuedRequests acquires mutex | 5.8 |
| REQ-PGPP-041 | Workers catch all exceptions | 6.2 |
| REQ-PGPP-042 | Connection loss triggers PQreset | 6.2 |
| REQ-PGPP-043 | Re-prepare after reconnect | 6.2 |
| REQ-PGPP-044 | busyWorkers atomic increment/decrement | 6.2 |
| REQ-PGPP-045 | FireAndForget never leaks frame | 7.1 |
| REQ-PGPP-046 | FireAndForget catches unhandled exceptions | 7.1 |
| REQ-PGPP-047 | Coroutine resumes on worker thread | 7.2 |
| REQ-PGPP-048 | Arguments captured by value | 7.2 |
| REQ-PGPP-049 | Result rows moved out in await_resume | 7.3 |
| REQ-PGPP-050 | Factory functions decay argument types | 7.4 |
| REQ-PGPP-051 | Backward-compat aliases forward identically | 7.4 |
| REQ-PGPP-052 | No-op logging is zero cost | 8 |
| REQ-PGPP-053 | PGPP_USE_ALOG auto-set by CMake | 8 |
| REQ-PGPP-054 | Builds without alog | 9 |
| REQ-PGPP-055 | PostgreSQL found via find_package | 9 |
| REQ-PGPP-056 | Supports PostgreSQL 13-17 | 9 |
