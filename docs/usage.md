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

Callback runs directly on the DB worker thread — no blocking, no extra threads.

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

Auto-commits on success, auto-rollbacks on exception:

```cpp
auto f = db.transaction([&](PgppConnection& conn) {
    conn.execRaw("UPDATE wallet SET balance = balance - 100 WHERE user_id = '1'");
    conn.execRaw("UPDATE wallet SET balance = balance + 100 WHERE user_id = '2'");
    // exception here → automatic ROLLBACK
});

bool committed = f.get().value_or(false);
```

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
db.totalConnections();  // pool size
db.freeConnections();   // idle workers
db.busyConnections();   // executing queries
db.queuedRequests();    // waiting in queue
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
the sync wrappers already treat that as `false`. A callback that throws is caught: on a worker
thread it is logged, on the calling thread it is dropped.

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
