# Usage Guide

## Synchronous API

Blocking calls — simplest to use, suitable for scripts and initialization code.

```cpp
#include <pgpp/pgpp.h>

// INSERT / UPDATE / DELETE
bool ok = db.execSync("insert_account", login, password, email);

// SELECT — returns typed rows
using Row = std::tuple<std::string, std::string>;
auto [ok, rows] = db.querySync<Row>("find_account", login);
```

## Future-based API

Fire a query, do other work, collect the result later.

```cpp
auto f = db.execAsync("insert_account", login, password, email);
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
}, login, password);
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
    using Row = std::tuple<std::string, std::string>;
    auto [ok, rows] = co_await coQuery<Row>(db, "find_account", login);

    if (!ok || !*ok || rows.empty()) {
        onResult(false);
        co_return;
    }

    auto& [dbLogin, dbPassword] = rows[0];
    onResult(dbPassword == password);
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
auto f = db.transaction([&](PgppConnection& conn) {
    return conn.execRaw("UPDATE wallet SET balance = balance - 100 WHERE user_id = '1'")
        && conn.execRaw("UPDATE wallet SET balance = balance + 100 WHERE user_id = '2'");
});

bool committed = f.get().value_or(false);   // nullopt only when the pool is shutting down
```

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
returned, so it can neither swallow the next caller's writes nor stay unusable. Session settings
made with `SET` are not reset; use `SET LOCAL` inside a transaction for per-request settings.

## Type Mapping

Results arrive in PostgreSQL's binary format and are decoded straight into the tuple; nothing is
parsed from text. The column type must match the C++ type:

| PostgreSQL column | C++ type | OID constant (for parameters) |
|---|---|---|
| TEXT, VARCHAR, CHAR(n), NAME, JSON, JSONB, XML, untyped literal | `std::string` | `pg::TEXT`, `pg::VARCHAR`, `pg::BPCHAR`, ... |
| SMALLINT, INTEGER, BIGINT, OID | `int16_t`, `int`, `int64_t`, `uint32_t` (any integral type; widened or range-checked) | `pg::INT2`, `pg::INT4`, `pg::INT8`, `pg::OID` |
| REAL, DOUBLE PRECISION (and the integer types) | `float`, `double` | `pg::FLOAT4`, `pg::FLOAT8` |
| BOOLEAN | `bool` | `pg::BOOL` |

Anything else needs a cast in the SQL:

```cpp
// timestamp / uuid / numeric into std::string: cast to text
db.prepareStatement({"created", "SELECT created_at::text FROM account WHERE id = $1", {pg::INT4}});
// numeric into a number: cast to a binary-decodable type
db.prepareStatement({"balance", "SELECT balance::float8 FROM wallet WHERE id = $1", {pg::INT4}});
```

A value that does not fit the C++ type fails the query (`false`, no rows) rather than wrapping:
`COUNT(*)` above `INT_MAX` read into `int`, 40000 read into `int16_t`, -1 read into `uint32_t`.
Parameters are still passed as text strings. NULL values are left at their C++ default
(empty string, 0, false).
