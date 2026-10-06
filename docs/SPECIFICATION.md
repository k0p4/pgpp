# pgpp Specification

> C++23 PostgreSQL Connection Pool Library

## Overview

pgpp is a thread-safe PostgreSQL connection pool library built on libpq. It provides
synchronous, future-based, callback-based, and coroutine-based interfaces for executing
prepared statements and raw SQL. Connections live on a free-list and are handed out as leases;
a small executor runs the asynchronous APIs. Lost connections are restored automatically.

**License:** GPL-3.0-or-later

---

## 1. Data Structures

### 1.1 PgppConnectionInfo

Configuration struct for database connection parameters.

| Field | Type | Default | Description |
|---|---|---|---|
| `dbname` | `std::string` | `""` | Database name (required) |
| `host` | `std::string` | `""` | Server hostname |
| `sslmode` | `std::string` | `""` | SSL mode (disable, allow, prefer, require, verify-ca, verify-full). Empty = libpq's default, `prefer`: encrypted if the server offers it, server identity not verified. Production deployments should set `verify-full` and pass `sslrootcert` through `libpqParams`. |
| `options` | `std::string` | `""` | Server command-line options, e.g. `-c statement_timeout=5000` |
| `user` | `std::string` | `""` | Database user |
| `password` | `std::string` | `""` | User password. Used to connect and not kept by the pool afterwards. |
| `port` | `uint16_t` | `5432` | Server port |
| `connectTimeoutSeconds` | `int` | `0` | `connect_timeout`; 0 = libpq's default (no limit) |
| `libpqParams` | `std::string` | `""` | Any further libpq keywords, verbatim (`sslrootcert='...' channel_binding=require`). Appended last, so it can override the fields above. |
| `acquireTimeout` | `std::chrono::milliseconds` | 30000 | How long `acquire()` and the synchronous API wait for a free connection; 0 = no limit |
| `maxQueuedRequests` | `size_t` | `0` | Upper bound on requests waiting for an executor thread; 0 = unbounded |
| `resetSessionAfterRequest` | `bool` | `false` | Reset the session every time a connection is returned (REQ-PGPP-080) |

**Requirements:**

- **REQ-PGPP-001:** `dbname` must be non-empty; `buildConnectionString` returns empty string if `dbname` is empty.
- **REQ-PGPP-002:** Special characters in field values (`'` and `\`) must be escaped in the connection string using backslash escaping within single-quoted values.
- **REQ-PGPP-003:** `password` is only included in the connection string if `user` is also non-empty.
- **REQ-PGPP-077:** `connectTimeoutSeconds > 0` is emitted as `connect_timeout=N`. `libpqParams` is appended verbatim and last; libpq takes the last occurrence of a keyword, so a keyword given there overrides the generated one. `PgppPool::buildConnectionString` is a public static function (the only way to see the string the pool uses; it contains the password).

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
| `task` | `std::function<void(PgppPool::Lease)>` | Work function executed on an executor thread with a leased connection. Receives an empty lease if the pool is shutting down or the connection could not be restored. The task returns the lease (`release()`) before completing its future/callback/coroutine (REQ-PGPP-069). |

---

## 2. pg:: Namespace OID Constants

Type OID constants matching the PostgreSQL system catalog (`pg_type.oid`). Used in `Statement::variables`.

| Constant | Value | PostgreSQL Type |
|---|---|---|
| `pg::BOOL` | 16 | boolean |
| `pg::BYTEA` | 17 | bytea |
| `pg::CHAR` | 18 | "char" |
| `pg::NAME` | 19 | name |
| `pg::INT8` | 20 | bigint (int8) |
| `pg::INT2` | 21 | smallint (int2) |
| `pg::INT4` | 23 | integer (int4) |
| `pg::TEXT` | 25 | text |
| `pg::OID` | 26 | oid |
| `pg::JSON` | 114 | json |
| `pg::XML` | 142 | xml |
| `pg::FLOAT4` | 700 | real (float4) |
| `pg::FLOAT8` | 701 | double precision (float8) |
| `pg::UNKNOWN` | 705 | unknown (untyped string literal) |
| `pg::BPCHAR` | 1042 | character(n) |
| `pg::VARCHAR` | 1043 | varchar |
| `pg::DATE` | 1082 | date |
| `pg::TIME` | 1083 | time |
| `pg::TIMESTAMP` | 1114 | timestamp |
| `pg::TIMESTAMPTZ` | 1184 | timestamptz |
| `pg::TIMETZ` | 1266 | timetz |
| `pg::NUMERIC` | 1700 | numeric |
| `pg::CSTRING` | 2275 | cstring |
| `pg::JSONB` | 3802 | jsonb |

Legacy OID macros (e.g., `TEXTOID`, `INT4OID`) are available only when `PGPP_ENABLE_OID_MACROS` is defined before including the header.

**Requirements:**

- **REQ-PGPP-006:** OID values must match the PostgreSQL system catalog exactly.
- **REQ-PGPP-007:** Legacy macros must delegate to `pg::` namespace constants (not hardcoded duplicates).

---

## 3. Result Decoding (binary format)

The result overload of `execPrepared` asks libpq for results in **binary format** (REQ-PGPP-062), so
every cell is the PostgreSQL wire encoding of its column type: integers and IEEE floats in network byte
order, booleans as one byte, textual types as the raw text. Nothing is parsed from text.
`Internal::Details::decodeBinary<T>(oid, bytes)` decodes one cell into the requested C++ type and returns
`std::expected<T, ConversionError>` (`Converted<T>`); it never throws.

`ConversionError` is one of:

| Value | Meaning |
|---|---|
| `TypeMismatch` | the column's PostgreSQL type cannot be read into the requested C++ type |
| `OutOfRange` | the value does not fit the requested C++ type |
| `Malformed` | the cell is not the size its type's binary encoding requires |

| C++ type | Accepted column types | Rule |
|---|---|---|
| `std::string` | `char`, `name`, `text`, `json`, `xml`, `unknown`, `bpchar`, `varchar`, `cstring`, `jsonb` (version byte stripped) | bytes copied as-is. Any other column (int, timestamp, uuid, numeric, ...) is `TypeMismatch`: cast to `::text` in the SQL. |
| any integral type (`int`, `int16_t`, `int64_t`, `uint32_t`, ...) except `bool` | `int2`, `int4`, `int8`, `oid` | widened to 64 bits, then range-checked against `T` with `std::in_range`: 40000 into `int16_t`, or -1 / 5000000000 into `uint32_t`, is `OutOfRange`, never a silent wrap. |
| `double`, `float` | `float4`, `float8`, and the integer types above | `float8` into `float` is `OutOfRange` when finite and above `FLT_MAX`; infinities and NaN pass through. |
| `bool` | `boolean` | one byte, non-zero is `true`. |
| `std::optional<T>` for any `T` above | as `T` | a non-NULL cell decodes as `T` (same errors); a NULL cell is `nullopt`. |

`numeric` has no decoder: select it as `::int8` or `::float8`. NULL cells never reach a decoder.

**Requirements:**

- **REQ-PGPP-008:** NULL values (detected via `PQgetisnull`) must leave the target at its default-constructed value (`""`, `0`, `false`, or `nullopt` for a `std::optional` column); the decoder is never called for NULL fields.
- **REQ-PGPP-075:** A result column of type `std::optional<T>` makes NULL distinguishable: NULL reads as `nullopt`, anything else as `T` with `T`'s decoding rules. A parameter of type `std::optional<std::string>` (or `std::optional` of any string-like type) is sent as SQL NULL when empty, as its value otherwise.
- **REQ-PGPP-009:** A decoder must accept exactly the column types listed above for its C++ type and reject every other column type with `TypeMismatch`; it must verify the cell size of fixed-width types and report `Malformed` otherwise.
- **REQ-PGPP-057:** A cell that cannot be decoded (`TypeMismatch`, `OutOfRange` or `Malformed`) fails the whole query: `fillTupleFromPQValues` returns `false` at the first failing column, and the result overload of `execPrepared` logs the row via `logConversionError`, removes any rows it already appended, and returns `false`. No partial result and no exception ever reaches callers, futures, callbacks, or coroutines.

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
| `execPrepared` (no results) | `template<typename... Ts> bool execPrepared(name, args...)` | Executes a prepared statement and discards whatever it returns: `true` on `PGRES_COMMAND_OK` (INSERT, UPDATE, DELETE) or `PGRES_TUPLES_OK` (SELECT, `INSERT ... RETURNING`), REQ-PGPP-073. |
| `execPrepared` (with results) | `template<typename... Ts, typename... TAs> bool execPrepared(name, vector<tuple<TAs...>>&, args...)` | Executes a prepared statement expecting `PGRES_TUPLES_OK` (SELECT) with binary results (REQ-PGPP-062). Looks up each column's type OID once, checks the column count (REQ-PGPP-063), then appends rows to the result vector; on a decoding failure appends nothing and returns `false` (REQ-PGPP-057). |

Arguments are `std::string` (anything with `.c_str()` and `.size()`), or `std::optional` of one
(REQ-PGPP-075).

**Requirements:**

- **REQ-PGPP-015:** `execRaw` must return `false` if the connection is not open.
- **REQ-PGPP-016:** `execPrepared` passes all arguments as text-format strings (`Internal::Details::paramValue`: `.c_str()`, or `nullptr` for an empty `std::optional`, which libpq sends as NULL).
- **REQ-PGPP-073:** The no-result overload treats `PGRES_TUPLES_OK` as success: a statement that returns rows through `execSync`, `execAsync`, `exec` or `coExec` ran, and its rows are dropped. Every other status is a failure.
- **REQ-PGPP-074:** A text parameter whose length differs from `strlen(c_str())`, i.e. one with an embedded NUL, is refused by both overloads before anything is sent: `false`, logged via `logParamError`. Text cannot carry a NUL and silently truncating at it would change the query (CWE-158).
- **REQ-PGPP-017:** The result overload of `execPrepared` must append to (not replace) the result vector, using `reserve` for efficiency.
- **REQ-PGPP-018:** Failed executions must log the error via `logTemplateError`.
- **REQ-PGPP-062:** The result overload of `execPrepared` requests binary results (`PQexecPrepared` result format 1). Parameters remain text (REQ-PGPP-016). Column types are read with `PQftype` once per result and passed to the decoders.
- **REQ-PGPP-063:** If the result has fewer columns than the requested tuple, `execPrepared` logs via `logColumnCountError` and returns `false` without decoding. Extra columns are ignored.

---

## 5. PgppPool

Thread-safe connection pool built from two standard parts: a free-list of connections handed
out as RAII leases, and an executor (a fixed set of threads over one queue) for the asynchronous
APIs. No thread owns a connection; whoever holds a lease uses it on their own thread.

### 5.1 Lifecycle

| Method | Signature | Description |
|---|---|---|
| Constructor | `PgppPool()` | Sets the default pool size to `min(hardware_concurrency(), 8)`, or 8 when the core count is unknown (REQ-PGPP-079) |
| Destructor | `~PgppPool()` | Calls `shutdown()` |
| `initialize` | `bool initialize(const PgppConnectionInfo&, size_t poolSize=0)` | Builds the connection string, opens `poolSize` connections, prepares the registered statements on each, starts `poolSize` executor threads, stores `acquireTimeout` and `maxQueuedRequests`. If `poolSize > 0`, overrides the default. |
| `shutdown` | `void shutdown()` | Stops accepting work, cancels the statement in flight on every leased connection (REQ-PGPP-076), lets the executor drain pending requests (each completes with `nullopt`), joins the executor threads, waits for outstanding leases, closes the connections. From an executor thread (inside a callback, transaction work or a resumed coroutine): stops, cancels and returns without waiting (REQ-PGPP-070). |
| `isInitialized` | `bool isInitialized() const noexcept` | Atomic check |

**Requirements:**

- **REQ-PGPP-019:** `initialize` must be idempotent -- second call returns `true` without reinitializing.
- **REQ-PGPP-020:** `shutdown` must be safe to call multiple times.
- **REQ-PGPP-021:** `shutdown` must drain all pending requests: each task runs with an empty lease so that futures receive `std::nullopt`, callbacks fire with `nullopt`, coroutines resume with `nullopt`. The executor threads drain the queue themselves after being told to stop, so a `shutdown()` that cannot join (REQ-PGPP-070) still resolves everything pending.
- **REQ-PGPP-022:** *Retired.* Tasks are never run under a lock (they run on executor threads that hold no pool lock), so drain-time re-entry into `enqueueRaw` cannot deadlock; it is refused because the pool is no longer initialized.
- **REQ-PGPP-023:** Destructor must call `shutdown()`.
- **REQ-PGPP-024:** Copy construction and copy assignment are deleted.
- **REQ-PGPP-060:** `shutdown()` must never hang, however soon after `initialize()` it is called. Every wait in the pool (`acquire`, the executor's wait for a task, the wait for leases) has the stop flag in its predicate, and every flag is set under the mutex its waiters use, so no wake-up can be lost.
- **REQ-PGPP-070:** A thread cannot join itself. `shutdown()` called on an executor thread marks the pool uninitialized (new requests are refused), sets the stop flags, wakes everyone and returns at once. The executor threads drain the queue (REQ-PGPP-021) and exit on their own; a leased connection is closed when its lease is returned; the next `shutdown()` or `initialize()` from a non-executor thread joins the threads and closes what is left. `shutdown()` from a non-executor thread is synchronous: when it returns, no executor thread runs, every pending request is resolved, every lease has been returned and every connection is closed. Destroying the pool from an executor thread is forbidden (the destructor cannot wait for the thread it runs on).
- **REQ-PGPP-071:** `initialize()` and `shutdown()` from non-executor threads are serialised by `m_lifecycleMutex`; two threads calling them concurrently leave the pool either fully initialized with exactly `poolSize` connections or fully shut down, never a mixture. Executor threads never take this mutex (a `shutdown()` that is joining them may hold it): on an executor thread `initialize()` reports the current state and `shutdown()` follows REQ-PGPP-070.
- **REQ-PGPP-076:** `shutdown()` does not wait for a running statement. After the stop flags are set it sends a cancel request (`PQcancel`, from a `PGcancel` handle kept per connection under `m_mutex` and refreshed after every reconnect) to every leased connection, whether leased by an executor task or by a caller's `acquire()`. The cancelled statement fails with PostgreSQL's "canceling statement due to user request" and the request reports `false` (not `nullopt`: it was executed). A connection idle under its lease is unaffected.
- **REQ-PGPP-079:** The default pool size is one connection per hardware thread, capped at 8, and 8 when `hardware_concurrency()` is 0. The database server's connection limit, not the client's core count, is the scarce resource; larger pools are an explicit choice through `poolSize`.

### 5.2 Statement Management

| Method | Signature | Description |
|---|---|---|
| `prepareStatement` | `bool prepareStatement(const Statement&)` | Appends the statement to the registered list and returns `true`; `false` if it could not be stored (allocation), in which case no connection will prepare it. Nothing else: every connection catches up on its next acquisition. |

**Requirements:**

- **REQ-PGPP-025:** Statements registered before `initialize` are prepared during connection creation.
- **REQ-PGPP-026:** Each connection remembers how many statements of the list it has prepared. On every acquisition (REQ-PGPP-067) it prepares the statements registered since, and only those, before the lease is handed out. A statement registered immediately after `initialize()` returns is therefore usable by the very next request, and a statement is never prepared twice on a connection (no `already exists` errors).
- **REQ-PGPP-027:** Statement list is protected by `m_stmtMutex`.
- **REQ-PGPP-059:** *Retired.* There are no worker threads to race with `prepareStatement`; REQ-PGPP-026 covers the guarantee.

### 5.3 Leases and the Synchronous API

| Method | Signature | Description |
|---|---|---|
| `acquire` | `[[nodiscard]] Lease acquire()` | Takes a free connection for the calling thread, reconnecting and catching up on statements first (REQ-PGPP-026, 042, 043). Waits up to `PgppConnectionInfo::acquireTimeout` (default 30 s; 0 = no limit). Returns an empty lease if none became free in time, the pool is not running, or the reconnect failed. |
| `Lease` | movable RAII handle | `operator bool`, `get()`, `operator->`, `operator*`, `release()`. Returns the connection to the pool on destruction or `release()`. |
| `execSync` | `template<Ts...> bool execSync(name, args...)` | `acquire()`, `execPrepared`, release. `false` if no lease. |
| `querySync` | `template<RowTuple, Ts...> pair<bool, vector<RowTuple>> querySync(name, args...)` | Same with the result overload. `{false, {}}` if no lease. |
| `execRawSync` | `bool execRawSync(const string& sql)` | Same with `execRaw`. |
| `transactionSync` | `template<F> bool transactionSync(F&& work)` | `acquire()`, then the transaction protocol of 5.6 on the calling thread; `work` is called in place, not copied. `false` if no lease. |

**Requirements:**

- **REQ-PGPP-067:** A connection is used only by the holder of its lease, on the holder's thread. The pool hands out a connection only through `acquire()`; the executor acquires one per task. `acquire()` reconnects a lost connection and prepares missing statements on the caller's thread, outside the pool lock. A caller that already holds a lease (inside `transaction()` work, or with an explicit `acquire()`) and calls the pool again gets a *second* connection, independent of the first, or an empty lease after `acquireTimeout` when none is free; at pool size 1 this is how a nested call fails instead of hanging.
- **REQ-PGPP-028:** The synchronous methods run on the calling thread with their own lease and never use the executor. They cannot wait for an executor thread, so they are safe from inside a callback, transaction work or a resumed coroutine.

### 5.4 Future-Based API

| Method | Signature | Return |
|---|---|---|
| `execAsync` | `template<Ts...> future<optional<bool>> execAsync(name, args...)` | `nullopt` on shutdown/no connection; `true`/`false` on success/failure |
| `queryAsync` | `template<RowTuple, Ts...> future<pair<optional<bool>, vector<RowTuple>>> queryAsync(name, args...)` | `{nullopt, {}}` on shutdown/no connection |
| `execRawAsync` | `future<optional<bool>> execRawAsync(const string& sql)` | `nullopt` on shutdown/no connection |

**Requirements:**

- **REQ-PGPP-029:** If the pool is not running at call time, the future is resolved with `nullopt` at once (the request is refused by `enqueueRaw`).
- **REQ-PGPP-029a:** If the request cannot be built or queued (allocation failure, an argument whose copy throws), the future is resolved with `nullopt`; if not even that promise can be allocated, an invalid future is returned (REQ-PGPP-061).
- **REQ-PGPP-030:** If `enqueueRaw` fails (returns `false`), the promise must be resolved with `nullopt`.
- **REQ-PGPP-031:** If the task receives an empty lease (drained at shutdown, or the connection could not be restored), the promise must be resolved with `nullopt`.
- **REQ-PGPP-069:** An executor task returns its lease *before* completing the future, firing the callback or resuming the coroutine. The connection is back in the pool when user code runs, so a sync call from a callback can take it, even at pool size 1. (The exception is `transaction()`, whose lease is held for the whole `work` by definition.)

### 5.5 Callback-Based API

| Method | Signature | Description |
|---|---|---|
| `exec` | `template<Ts...> void exec(name, callback, args...)` | Callback fires on an executor thread with `optional<bool>` |
| `query` | `template<RowTuple, Ts...> void query(name, callback, args...)` | Callback fires on an executor thread with `optional<bool>` and `vector<RowTuple>` |

**Requirements:**

- **REQ-PGPP-032:** A callback runs on the executor thread that executed the request, after the request's connection was returned (REQ-PGPP-069); on the calling thread, with `nullopt`, when the request could not be queued (REQ-PGPP-033); on an executor thread, with `nullopt`, when the request was drained at shutdown. A request issued from a callback is queued like any other and runs later, on whichever executor thread picks it up.
- **REQ-PGPP-033:** If shutting down, enqueue fails, or the request cannot be built, the callback receives `nullopt` immediately on the calling thread, exactly once. An exception thrown by the callback there is caught and dropped (REQ-PGPP-061).
- **REQ-PGPP-041:** The executor catches all exceptions from tasks (both `std::exception` and `...`), logs them and keeps serving; a lease still held by the throwing task is returned by RAII.
- *Rule for callers:* do not block on a future from inside a callback. That blocks an executor thread on work queued to the same executor, which at pool size 1 is the thread itself. The synchronous API (REQ-PGPP-028) is the right tool there.

### 5.6 Transactions

| Method | Signature | Description |
|---|---|---|
| `transaction` | `template<F> future<optional<bool>> transaction(F&& work)` | On the executor, with its own lease: `BEGIN`, `work(PgppConnection&)` (returning `void` or a value convertible to `bool`), `COMMIT`. `ROLLBACK` and `false` if `work` threw, returned `false`, or left the connection in any state other than a healthy open transaction (a failed statement aborts it). `true` only if PostgreSQL reports `COMMIT`. `nullopt` if no lease. |
| `transactionSync` | `template<F> bool transactionSync(F&& work)` | The same protocol on the calling thread (5.3). |

Inside `work`, use the connection you were given. A pool call from there acquires a second
connection (REQ-PGPP-067): it is autocommit, independent of the transaction, and it fails after
`acquireTimeout` when no other connection is free. A `transaction()` started from inside `work`
is likewise an independent transaction on its own connection.

**Requirements:**

- **REQ-PGPP-034:** If `BEGIN` fails (including a null result), resolve with `false` without calling the work function.
- **REQ-PGPP-035:** Any exception thrown by `work` must trigger `ROLLBACK` and resolve with `false`.
- **REQ-PGPP-036:** `COMMIT` counts as successful only if the result is non-null, `PQresultStatus == PGRES_COMMAND_OK` **and** `PQcmdStatus` equals `"COMMIT"`. PostgreSQL answers a `COMMIT` on an aborted transaction with `PGRES_COMMAND_OK` and the command tag `ROLLBACK`; that must resolve with `false`.
- **REQ-PGPP-065:** After `work` returns, if `PQtransactionStatus` is not `PQTRANS_INTRANS` (a failed statement left it `PQTRANS_INERROR`; `work` ended the transaction itself; the connection is bad), issue `ROLLBACK` and resolve with `false` without attempting `COMMIT`.
- **REQ-PGPP-066:** If `work` returns a value convertible to `bool` and it converts to `false`, issue `ROLLBACK` and resolve with `false`. A `void` work behaves as one that returns `true`.

### 5.7 Raw Enqueue

| Method | Signature | Description |
|---|---|---|
| `enqueueRaw` | `bool enqueueRaw(unique_ptr<PgppRequest>)` | Queues a task on the executor. Returns `false` if the pool is not running. Public for coroutine awaitables. |

**Requirements:**

- **REQ-PGPP-037:** Must hold `m_taskMutex` during the push, then `notify_one`.
- **REQ-PGPP-038:** Must return `false` (without enqueuing) if the pool is not initialized or the executor is stopping.
- **REQ-PGPP-078:** With `maxQueuedRequests > 0`, `enqueueRaw` returns `false` when that many requests are already waiting (requests being executed do not count). The caller's API then reports the refusal at once: a future resolved with `nullopt`, a callback fired with `nullopt` on the calling thread, an awaitable completing with `nullopt` (REQ-PGPP-030/033).

### 5.8 Pool Statistics

| Method | Return | Description |
|---|---|---|
| `totalConnections()` | `size_t`, `noexcept` | Number of connections open in the pool |
| `freeConnections()` | `size_t`, `noexcept` | Connections on the free-list |
| `busyConnections()` | `size_t`, `noexcept` | Connections currently leased |
| `queuedRequests()` | `size_t`, `noexcept` | Tasks waiting in the executor queue (0 if the lock fails) |

**Requirements:**

- **REQ-PGPP-039:** `freeConnections` must never underflow; `free + busy == total` under the pool lock.
- **REQ-PGPP-040:** The getters read under the mutex that guards what they report (`m_mutex` for connections, `m_taskMutex` for the queue), so they are consistent and race-free against `initialize`/`shutdown`.

---

## 6. Threading Model

### 6.1 Shared state (complete inventory)

| State | Guard |
|---|---|
| free-list, leased count, stop flag, acquire timeout | `m_mutex` + `m_idleAvailable` |
| executor queue, executor stop flag, executor thread ids | `m_taskMutex` + `m_taskAvailable` |
| registered statements | `m_stmtMutex` |
| `initialize` / `shutdown` from non-executor threads, the thread handles | `m_lifecycleMutex` |
| `m_initialized` | atomic |

No thread-local state, no nesting counters. The lock order is `m_lifecycleMutex` → any other;
`m_mutex` and `m_taskMutex` are never held together.

### 6.2 Executor loop

```
loop:
    lock(taskMutex); wait(taskAvailable, predicate: stopping || !queue.empty)
    if queue empty: exit thread                 (stopping and drained)
    pop request; stopping = taskStopping; unlock
    lease = stopping ? empty : acquire(no timeout)   (wakes empty when shutdown begins)
    try: request.task(lease)                    (the task returns the lease before completing, REQ-069)
    catch: log
```

### 6.3 Acquisition and return

```
acquire(timeout):
    lock(mutex); if not running: return empty
    wait(idleAvailable, predicate: stopping || !idle.empty, timeout); if stopping/timeout: return empty
    pop slot; leased++; unlock
    if connection lost: PQreset; if still lost: return slot, return empty   (REQ-042)
    prepare statements registered since this connection last caught up     (REQ-026, 043)
    return lease

release(slot):
    if transaction open or aborted: ROLLBACK, warn                           (REQ-072)
    if resetSessionAfterRequest: reset the session; on failure reconnect     (REQ-080)
    lock(mutex); leased--; stopping ? close : push idle; unlock; notify_all
```

**Requirements:**

- **REQ-PGPP-042:** On connection loss, `acquire` must attempt `PQreset` on the caller's thread. If the reset fails, the connection goes back to the free-list and the caller gets an empty lease (`false` / `nullopt`).
- **REQ-PGPP-043:** After a successful reconnection the connection's prepared count is reset, so every registered statement is prepared again before the lease is handed out.
- **REQ-PGPP-044:** *Retired.* `busyConnections()` is the leased count under `m_mutex`.
- **REQ-PGPP-072:** A connection is returned clean. If `PQtransactionStatus` is `PQTRANS_INTRANS` or `PQTRANS_INERROR` when a lease is released (a caller issued `BEGIN` through the raw API and never ended it, or an error aborted a transaction a caller left open), the pool issues `ROLLBACK` and logs a warning before the connection goes back to the free-list. A caller's stray transaction can therefore neither swallow the next caller's writes nor leave the connection unusable. Session settings (`SET`, `SET ROLE`, temporary tables) are *not* reset by default: connections have no caller affinity, so use `SET LOCAL` inside `transaction()` for per-request settings, or enable REQ-PGPP-080.
- **REQ-PGPP-080:** With `PgppConnectionInfo::resetSessionAfterRequest`, every release additionally runs, in one round trip, `SET SESSION AUTHORIZATION DEFAULT; RESET ALL; CLOSE ALL; UNLISTEN *; SELECT pg_advisory_unlock_all(); DISCARD TEMP; DISCARD SEQUENCES` (what `DISCARD ALL` does minus `DEALLOCATE ALL`, so prepared statements survive, and minus `DISCARD PLANS`). If that fails the connection is reset (new session, statements prepared again on the next acquisition). The cost is one server round trip per request; the default is off.

---

## 7. Coroutines (pgpp_coroutines.h)

C++20 coroutine integration for `PgppPool`.

### 7.1 FireAndForget

Minimal coroutine return type for fire-and-forget async tasks.

- `initial_suspend` returns `suspend_never` (starts immediately).
- `final_suspend` returns `suspend_never` (self-destructs on completion).
- `unhandled_exception` catches and logs to `stderr`.
- `get_return_object_on_allocation_failure` makes frame allocation non-throwing: on failure the coroutine body never runs and the call returns normally (REQ-PGPP-061).
- A coroutine lambda's closure is referenced, not copied, by the frame: callers must keep it alive until the coroutine completes (a named variable, or a plain coroutine function whose parameters are copied into the frame). An immediately-invoked temporary lambda is a dangling-closure bug.

**Requirements:**

- **REQ-PGPP-045:** Must never leak the coroutine frame -- `final_suspend` must return `suspend_never`.
- **REQ-PGPP-046:** Unhandled exceptions must be caught and logged, never propagated.

### 7.2 DbExecAwaitable<Ts...>

Awaitable for executing a prepared statement without results.

- `await_ready` returns `false` (always suspends).
- `await_suspend` creates a `PgppRequest`, enqueues it on the executor; the task runs the statement with its lease, returns the lease, then resumes the coroutine handle on the executor thread.
- `await_resume` returns `std::optional<bool>` (`nullopt` if connection was `nullptr`).
- State (statement, arguments, result) lives inline in the awaitable, i.e. in the awaiting coroutine's frame; the request's task captures `this`. The caller must keep the coroutine alive until it is resumed (FireAndForget does so by construction).
- `await_suspend`, `await_ready` and the state are shared with `DbResultAwaitable` via `Internal::DbAwaitableBase` (CRTP).
- `coExec` / `coQuery` are `noexcept`: if the statement or an argument cannot be copied into the awaitable, or the request cannot be built in `await_suspend`, the awaitable completes immediately with `nullopt` (REQ-PGPP-061).

**Requirements:**

- **REQ-PGPP-047:** The coroutine resumes on the executor thread that executed the request (not the original thread), with the request's connection already back in the pool (REQ-PGPP-069); at shutdown it resumes with `nullopt` on the draining executor thread.
- **REQ-PGPP-048:** Arguments are captured by value in a `std::tuple` to ensure lifetime safety.
- **REQ-PGPP-058:** The awaiting coroutine is resumed exactly once for every enqueued request; when the statement fails the result is `false`, when the request is drained at shutdown it is `nullopt`.

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

- CMake 3.20+, C++23 required (`std::expected`). The `pgpp` target declares `cxx_std_23` as a PUBLIC compile feature, so consumers (FetchContent included) inherit the standard without setting it themselves.
- Static library target: `pgpp`, also reachable as `pgpp::pgpp`.
- Public dependencies: `PostgreSQL::PostgreSQL` and `Threads::Threads` (via `find_package`).
- Optional private dependency: `alog` (auto-detected via CMake target existence).
- Public include directory: `include/`.
- Header files: `include/pgpp/pgpp.h`, `include/pgpp/pgpp_connection.h`, `include/pgpp/pgpp_coroutines.h`.
- Consumed via CMake `FetchContent` by downstream projects.
- `CMakePresets.json` uses schema version 3 (CMake 3.21+); the presets are for standalone development only, the library itself needs 3.20.
- Warnings: the library and the tests compile with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` (`/W4` on MSVC), PRIVATE, so nothing reaches consumers; `-DPGPP_WARNINGS_AS_ERRORS=ON` makes them errors (CI does).

**Requirements:**

- **REQ-PGPP-054:** The library must build without `alog` present (logging becomes no-op).
- **REQ-PGPP-055:** PostgreSQL client library (libpq) must be found via `find_package(PostgreSQL REQUIRED)`.
- **REQ-PGPP-056:** The library must support PostgreSQL versions 13 through 17. CI runs the integration suite against each (`PGPP_TEST_PG_IMAGE`).
- **REQ-PGPP-081:** The `pgpp` target links `Threads::Threads` publicly and carries no test-only compile definitions; the tests use only the public API.

---

## 10. Error Handling Policy

pgpp does not use exceptions. Every failure is reported through a return value: `bool` for
execution results, `std::optional<bool>` where "no result at all" (shutdown, no connection) must
be distinguishable from failure, and `std::expected<T, E>` where the reason matters (result decoding).

**Requirements:**

- **REQ-PGPP-061:** No exception leaves the library. Every public function of `PgppPool`, `PgppConnection` and the coroutine helpers is declared `noexcept`, and every operation inside the library that can throw (allocation while building a request or a result, copying a caller's arguments into a request, `std::thread` / mutex / condition-variable errors, `std::promise` operations, a user callback invoked on the calling thread) is wrapped in `try`/`catch` at the point where a failure value can still be produced. The failure values are: `false` for `bool` results and sync wrappers; a future already resolved with `nullopt` (or an *invalid* future, `valid() == false`, if even that promise cannot be allocated; the sync wrappers treat it as `false`); the callback fired exactly once with `nullopt`; an awaitable that completes immediately with `nullopt`; an empty string from `lastError`. Worker threads log and continue. Library code must not call standard functions whose only failure channel is an exception (`std::stoi` and friends); results are decoded from libpq's binary format. A user callback that throws on a worker thread is caught and logged there; on the calling thread it is caught and dropped. The log sink is called from these `noexcept` paths, including the handlers that report an allocation failure, so it must not throw: the stderr sink swallows stream errors, and an `alog` sink is required to be non-throwing. `shutdown()` (and therefore the destructor) must not be called from a worker thread, i.e. from inside a callback, a `transaction` body or a coroutine resumed by the pool: a thread cannot join itself, so that worker is detached and keeps running against a pool that is being torn down.
- **REQ-PGPP-064:** `initialize` must leave the pool uninitialized (no connections, no threads) when any step fails, including thread creation; `shutdown` must drain the queue without allocating, so it cannot fail for lack of memory.

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
| REQ-PGPP-009 | Decoders accept exactly the listed column types per C++ type | 3 |
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
| REQ-PGPP-021 | Shutdown drains pending requests with an empty lease (nullopt) | 5.1 |
| REQ-PGPP-022 | Retired (tasks never run under a lock) | 5.1 |
| REQ-PGPP-023 | Destructor calls shutdown | 5.1 |
| REQ-PGPP-024 | Non-copyable pool | 5.1 |
| REQ-PGPP-025 | Pre-init statements prepared during creation | 5.2 |
| REQ-PGPP-026 | Each connection prepares the statements registered since its last acquisition | 5.2 |
| REQ-PGPP-027 | Statement list mutex-protected | 5.2 |
| REQ-PGPP-028 | Sync methods run on the calling thread with their own lease; never the executor | 5.3 |
| REQ-PGPP-029 | Pool not running resolves future with nullopt | 5.4 |
| REQ-PGPP-030 | Enqueue failure resolves with nullopt | 5.4 |
| REQ-PGPP-031 | Empty lease resolves with nullopt | 5.4 |
| REQ-PGPP-032 | Callbacks run on an executor thread after the connection was returned; from a callback, requests queue | 5.5 |
| REQ-PGPP-033 | Shutdown/enqueue fail delivers nullopt to callback | 5.5 |
| REQ-PGPP-034 | BEGIN failure resolves with false | 5.6 |
| REQ-PGPP-035 | Exception triggers ROLLBACK | 5.6 |
| REQ-PGPP-036 | COMMIT succeeded only with status OK and tag "COMMIT" | 5.6 |
| REQ-PGPP-037 | enqueueRaw holds the task mutex during push | 5.7 |
| REQ-PGPP-038 | enqueueRaw returns false if not running | 5.7 |
| REQ-PGPP-039 | free + busy == total; never underflows | 5.8 |
| REQ-PGPP-040 | Statistics read under the guarding mutex | 5.8 |
| REQ-PGPP-041 | Executor catches all exceptions from tasks | 5.5 |
| REQ-PGPP-042 | Connection loss triggers PQreset in acquire | 6.3 |
| REQ-PGPP-043 | Re-prepare after reconnect | 6.3 |
| REQ-PGPP-044 | Retired (leased count under the pool mutex) | 6.3 |
| REQ-PGPP-045 | FireAndForget never leaks frame | 7.1 |
| REQ-PGPP-046 | FireAndForget catches unhandled exceptions | 7.1 |
| REQ-PGPP-047 | Coroutine resumes on the executing executor thread, connection already returned | 7.2 |
| REQ-PGPP-048 | Arguments captured by value | 7.2 |
| REQ-PGPP-049 | Result rows moved out in await_resume | 7.3 |
| REQ-PGPP-050 | Factory functions decay argument types | 7.4 |
| REQ-PGPP-051 | Backward-compat aliases forward identically | 7.4 |
| REQ-PGPP-052 | No-op logging is zero cost | 8 |
| REQ-PGPP-053 | PGPP_USE_ALOG auto-set by CMake | 8 |
| REQ-PGPP-054 | Builds without alog | 9 |
| REQ-PGPP-055 | PostgreSQL found via find_package | 9 |
| REQ-PGPP-056 | Supports PostgreSQL 13-17 | 9 |
| REQ-PGPP-057 | Decoding failure fails the query, no partial rows | 3 |
| REQ-PGPP-058 | Awaitable always resumes, even if execution throws | 7.2 |
| REQ-PGPP-059 | Retired (no worker threads; covered by REQ-PGPP-026) | 5.2 |
| REQ-PGPP-060 | Every stop flag is set under its waiters' mutex; shutdown never hangs | 5.1 |
| REQ-PGPP-061 | No exceptions: failures are return values (bool / optional / expected) | 10 |
| REQ-PGPP-062 | Results requested in binary format; parameters stay text | 4.3 |
| REQ-PGPP-063 | Fewer result columns than requested fails the query | 4.3 |
| REQ-PGPP-064 | initialize rolls back on any failure; shutdown drains without allocating | 10 |
| REQ-PGPP-065 | Aborted (or otherwise not open) transaction after work: ROLLBACK, false | 5.6 |
| REQ-PGPP-066 | work returning false: ROLLBACK, false | 5.6 |
| REQ-PGPP-067 | Connections are used only through leases; a nested call gets a second connection or times out | 5.3 |
| REQ-PGPP-068 | *(see REQ-PGPP-028)* sync API on the calling thread, never the executor | 5.3 |
| REQ-PGPP-069 | Executor tasks return the lease before completing the future / callback / coroutine | 5.4 |
| REQ-PGPP-070 | shutdown(): synchronous from outside, non-blocking from an executor thread; destructor never from one | 5.1 |
| REQ-PGPP-071 | initialize/shutdown serialised; executor threads never take the lifecycle lock | 5.1 |
| REQ-PGPP-072 | A connection is returned clean: stray open/aborted transaction rolled back on release | 6.3 |
| REQ-PGPP-073 | No-result execPrepared accepts a statement that returns rows (rows dropped) | 4.3 |
| REQ-PGPP-074 | A parameter with an embedded NUL is refused, nothing sent | 4.3 |
| REQ-PGPP-075 | std::optional result columns (NULL = nullopt) and parameters (nullopt = NULL) | 3 |
| REQ-PGPP-076 | shutdown() cancels the statement in flight on every leased connection | 5.1 |
| REQ-PGPP-077 | connect_timeout and verbatim libpq parameters; public static buildConnectionString | 1.1 |
| REQ-PGPP-078 | Bounded executor queue: excess requests refused at once with nullopt | 5.7 |
| REQ-PGPP-079 | Default pool size min(cores, 8) | 5.1 |
| REQ-PGPP-080 | Opt-in session reset on every release | 6.3 |
| REQ-PGPP-081 | Threads::Threads public, no test-only definitions in the library | 9 |
