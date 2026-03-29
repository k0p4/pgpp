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

## Type Mapping

| PostgreSQL | C++ type | OID constant |
|---|---|---|
| VARCHAR, TEXT | `std::string` | `pg::VARCHAR`, `pg::TEXT` |
| INTEGER | `int` | `pg::INT4` |
| BIGINT | `int64_t` | `pg::INT8` |
| SMALLINT | `int16_t` | `pg::INT2` |
| REAL | `float` | `pg::FLOAT4` |
| DOUBLE PRECISION | `double` | `pg::FLOAT8` |
| BOOLEAN | `bool` | — |

NULL values are left at their C++ default (empty string, 0, false).
