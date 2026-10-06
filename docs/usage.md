# Usage Guide

## Connecting

```cpp
#include <pgpp/pgpp.h>

PgppPool db;
PgppConnectionInfo info;
info.host     = "db.internal";
info.dbname   = "app";
info.user     = "app";
info.password = secretFromYourVault();
info.sslmode  = "verify-full";                       // libpq's default is "prefer": no server verification
info.libpqParams = "sslrootcert='/etc/ssl/root.crt'"; // any other libpq keyword, verbatim
info.connectTimeoutSeconds = 5;
db.initialize(info, 4);                              // poolSize 0 = one per core, at most 8
```

`libpqParams` is appended last, so a keyword given there overrides the fields above. The
password is used to connect and not kept by the pool. `PgppPool::buildConnectionString(info)`
shows the exact string (password included) if you need to debug a connection.

## Synchronous API

Blocking calls — simplest to use, suitable for scripts and initialization code.

```cpp
// INSERT / UPDATE / DELETE
bool ok = db.execSync("insert_account", login, passwordHash, email);

// SELECT — returns typed rows
using Row = std::tuple<std::string, std::string>;
auto [ok, rows] = db.querySync<Row>("find_account", login);
```

A statement that returns rows (`SELECT`, `INSERT ... RETURNING`) can also go through `execSync`
and the other no-result APIs: it reports `true` and the rows are dropped.

## Future-based API

Fire a query, do other work, collect the result later.

```cpp
auto f = db.execAsync("insert_account", login, passwordHash, email);
// ... do something else ...
auto result = f.get();  // std::optional<bool>

using Row = std::tuple<std::string, int>;
auto f2 = db.queryAsync<Row>("get_player_stats", login);
auto [ok, rows] = f2.get();
```

## Callback-based API

The callback runs on one of the pool's executor threads, after the statement's connection has
been returned to the pool.

```cpp
db.exec("delete_account", [](std::optional<bool> ok) {
    if (ok && *ok)
        std::cout << "Account deleted\n";
}, login);
```

## Coroutines

C++20 coroutine support via `<pgpp/pgpp_coroutines.h>`.

`FireAndForget` is a minimal coroutine return type — starts immediately, self-destructs on completion.

A coroutine lambda's closure must outlive the coroutine: keep the lambda in a named variable
until the coroutine has finished, or write a plain coroutine function (parameters are copied into
the frame). Invoking a temporary lambda, `[&]() -> FireAndForget { ... }();`, destroys the closure
at the end of the statement while the suspended frame still refers to its captures.

```cpp
#include <pgpp/pgpp_coroutines.h>

FireAndForget authenticateUser(PgppPool& db, std::string login,
                               std::string password,
                               std::function<void(bool)> onResult)
{
    using Row = std::tuple<std::string, std::string>;   // login, password hash
    auto [ok, rows] = co_await coQuery<Row>(db, "find_account", login);

    if (!ok || !*ok || rows.empty()) {
        onResult(false);
        co_return;
    }

    auto& [dbLogin, passwordHash] = rows[0];
    onResult(verifyPassword(password, passwordHash));   // argon2 / bcrypt; never store plaintext
}
```

## Raw SQL

For schema setup, migrations, or any non-prepared query:

```cpp
db.execRawSync("CREATE INDEX IF NOT EXISTS idx_account_email ON account(email)");

// Async variant
auto f = db.execRawAsync("VACUUM ANALYZE account");
```

## Transactions

`transaction` takes a connection, runs `BEGIN`, calls `work(conn)` on an executor thread, then
`COMMIT`; `transactionSync` does the same on the calling thread and returns the `bool`. The future
resolves `true` only if PostgreSQL actually committed. It resolves `false`, after a `ROLLBACK`, when:

- any statement inside `work` failed: PostgreSQL then leaves the transaction *aborted*, and a
  `COMMIT` on an aborted transaction silently answers `ROLLBACK`, so the pool checks the
  transaction state itself instead of trusting `COMMIT`;
- `work` returns `false` (the explicit, non-throwing way to abort);
- `work` throws.

`work` may return `void` or `bool`. Statement results inside `work` are plain `bool`s, so the
natural form is to chain them:

```cpp
db.prepareStatement({"move", "UPDATE wallet SET balance = balance + $2 WHERE user_id = $1", {pg::INT4, pg::INT8}});

auto f = db.transaction([=](PgppConnection& conn) {
    return conn.execPrepared("move", from, std::to_string(-amount))
        && conn.execPrepared("move", to, std::to_string(amount));
});

bool committed = f.get().value_or(false);   // nullopt only when the pool is shutting down
```

Inside `work` the connection offers the same `execPrepared` as the pool, so user input goes
through parameters there too; `execRaw` is for SQL you wrote yourself.

A `void` work is equivalent to one that always returns `true`: it still rolls back if a
statement failed, it just cannot abort on its own without throwing.

## Leases

The pool is a free-list of connections. Every API takes one for the duration of a request and
returns it. You can do the same yourself when several statements must share one connection
without a transaction:

```cpp
if (auto conn = db.acquire()) {           // waits up to PgppConnectionInfo::acquireTimeout
    conn->execRaw("SET LOCAL statement_timeout = 1000");
    conn->execPrepared("report", id);
}                                         // returned here
```

An empty lease means no connection became free in time (default 30 s, configurable through
`acquireTimeout`), or the pool is not running.

## Back-pressure

Requests for the asynchronous APIs wait in one queue for an executor thread. With
`PgppConnectionInfo::maxQueuedRequests` set, a request that finds the queue full is refused at
once: the future resolves with `nullopt`, the callback fires with `nullopt` on the calling thread.
The default (0) is unbounded.

## Shutdown

`shutdown()` stops accepting work, sends a cancel request to every connection that is executing
a statement (the request reports `false`), drains the queued requests with `nullopt`, waits for
outstanding leases and closes the connections. It never waits for a long statement to finish.

## Calling the Pool from a Callback or Transaction

Callbacks, `transaction()` work and coroutine bodies after `co_await` run on the pool's executor
threads. Three rules keep that simple:

- **The sync API is always fine.** `execSync`, `querySync`, `execRawSync` and `transactionSync`
  acquire a connection on the calling thread and never touch the executor, so they work from a
  callback even with a pool of one: the callback's own connection is already back in the pool.
- **Inside `transaction()` work, use the connection you were given.** A pool call from there
  takes a *second* connection: it is autocommit, independent of your transaction, and with a pool
  of one it fails after `acquireTimeout` rather than joining the transaction.
- **Do not block on a future inside a callback.** `execAsync(...).get()` there waits for an
  executor thread, and with a pool of one that is the thread you are on. Use the sync API, or
  chain another callback.

`shutdown()` from inside a callback returns immediately and finishes on the executor threads
themselves; the next `shutdown()` or the destructor, from one of your own threads, waits for them.
Never destroy the pool from a callback.

## Statement Preparation

Statements are prepared once and available on all pool connections.
Call `prepareStatement` before using `exec`/`query` APIs.

```cpp
db.prepareStatement({
    "get_leaderboard",                                    // name
    "SELECT login, score FROM scores "
    "ORDER BY score DESC LIMIT $1",                       // SQL with $N params
    { pg::INT4 }                                          // parameter OIDs
});

auto [ok, rows] = db.querySync<std::tuple<std::string, int>>(
    "get_leaderboard", std::to_string(10));
```

## Pool Statistics

```cpp
db.totalConnections();  // connections in the pool
db.freeConnections();   // on the free-list
db.busyConnections();   // currently leased
db.queuedRequests();    // tasks waiting for an executor thread
```

## Logging

By default, pgpp compiles with **no logging** (zero overhead). If your project provides the [alog](https://github.com/ihor-drachuk/alog) target, logging is enabled automatically:

```cmake
FetchContent_MakeAvailable(alog)   # provide alog first
FetchContent_MakeAvailable(pgpp)   # pgpp detects it
```

For simple stderr output without alog:

```cmake
target_compile_definitions(pgpp PRIVATE PGPP_USE_STDERR)
```

## Error Handling

pgpp never throws. Every public function is `noexcept` and reports failure through its return
value: `false`, `std::nullopt` (shutdown, no connection, or the request could not be built), an
already-resolved future, or an awaitable that completes at once. Check `future.valid()` only if
you want to distinguish the extreme case where not even the result promise could be allocated;
the sync wrappers already treat that as `false`. A callback that throws is caught: on an executor
thread it is logged, on the calling thread it is dropped.

A connection always comes back clean: if a caller left a transaction open or aborted (a raw
`BEGIN` without `COMMIT`, an error inside it), the pool rolls it back when the connection is
returned, so it can neither swallow the next caller's writes nor stay unusable.

## Session State

Connections have no caller affinity. Whatever one request does to its session (`SET`, `SET
ROLE`, `LISTEN`, an advisory lock, a temporary table) is seen by whoever gets that connection
next. Two ways to keep requests apart:

- `SET LOCAL` inside `transaction()` / `transactionSync()`: the setting ends with the
  transaction. Free.
- `PgppConnectionInfo::resetSessionAfterRequest = true`: every time a connection is returned
  the pool resets the session (`RESET ALL`, `SET SESSION AUTHORIZATION DEFAULT`, `UNLISTEN *`,
  advisory locks released, temporary tables and sequence state discarded; prepared statements
  stay). One extra round trip per request.

A parameter containing an embedded NUL byte is refused (`false`): PostgreSQL text cannot hold
one, and truncating at it would change the query.

## Type Mapping

Results arrive in PostgreSQL's binary format and are decoded straight into the tuple; nothing is
parsed from text. The column type must match the C++ type:

| PostgreSQL column | C++ type | OID constant (for parameters) |
|---|---|---|
| TEXT, VARCHAR, CHAR(n), NAME, JSON, JSONB, XML, untyped literal | `std::string` | `pg::TEXT`, `pg::VARCHAR`, `pg::BPCHAR`, ... |
| SMALLINT, INTEGER, BIGINT, OID | `int16_t`, `int`, `int64_t`, `uint32_t` (any integral type; widened or range-checked) | `pg::INT2`, `pg::INT4`, `pg::INT8`, `pg::OID` |
| REAL, DOUBLE PRECISION (and the integer types) | `float`, `double` | `pg::FLOAT4`, `pg::FLOAT8` |
| BOOLEAN | `bool` | `pg::BOOL` |
| any of the above, nullable | `std::optional<T>` (NULL reads as `nullopt`) | |

Anything else needs a cast in the SQL:

```cpp
// timestamp / uuid / numeric into std::string: cast to text
db.prepareStatement({"created", "SELECT created_at::text FROM account WHERE id = $1", {pg::INT4}});
// numeric into a number: cast to a binary-decodable type
db.prepareStatement({"balance", "SELECT balance::float8 FROM wallet WHERE id = $1", {pg::INT4}});
```

A value that does not fit the C++ type fails the query (`false`, no rows) rather than wrapping:
`COUNT(*)` above `INT_MAX` read into `int`, 40000 read into `int16_t`, -1 read into `uint32_t`.
NULL is left at the C++ default (empty string, 0, false) in a plain column and becomes `nullopt`
in a `std::optional` column.

Parameters are passed as text strings: `std::string` (or anything with `.c_str()` and
`.size()`), or `std::optional<std::string>` where `nullopt` is SQL NULL:

```cpp
db.prepareStatement({"set_score", "UPDATE account SET score = $2 WHERE login = $1", {pg::VARCHAR, pg::INT4}});
db.execSync("set_score", login, std::optional<std::string>{});        // score = NULL

using Row = std::tuple<std::string, std::optional<int>>;
auto [ok, rows] = db.querySync<Row>("find_score", login);              // nullopt when NULL
```
