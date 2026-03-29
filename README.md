# pgpp

C++ PostgreSQL connection pool with thread-per-connection architecture and multiple async APIs.

## Features

- **Thread-per-connection pool** — each worker owns its connection exclusively, no sharing
- **Multiple APIs** — sync, future, callback, C++20 coroutines
- **Auto-reconnect** — dead connections are restored transparently
- **Prepared statements** — registered once, available on all connections
- **Transactions** — auto-rollback on exception
- **Raw SQL** — for migrations, DDL, one-off queries
- **Optional logging** — zero-overhead no-op by default, alog integration available
- **C++20**, depends only on libpq

## Quick Start

### CMake (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(pgpp
  GIT_REPOSITORY https://github.com/k0p4/pgpp.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(pgpp)

target_link_libraries(myapp PRIVATE pgpp)
```

Requires PostgreSQL development headers (`libpq-dev` / `postgresql-devel`).

### Minimal Example

```cpp
#include <pgpp.h>
#include <iostream>

int main()
{
    PgppConnectionInfo info;
    info.host     = "127.0.0.1";
    info.port     = 5432;
    info.dbname   = "mydb";
    info.user     = "postgres";
    info.password = "secret";

    PgppPool db;
    db.initialize(info, 4);  // 4 worker threads, 4 connections

    // Create table
    db.execRawSync(
        "CREATE TABLE IF NOT EXISTS account ("
        "  login VARCHAR PRIMARY KEY,"
        "  password VARCHAR NOT NULL,"
        "  email VARCHAR NOT NULL"
        ")");

    // Prepare statements
    db.prepareStatement({"insert_account",
        "INSERT INTO account (login, password, email) VALUES ($1, $2, $3)",
        { pg::VARCHAR, pg::VARCHAR, pg::VARCHAR }});

    db.prepareStatement({"find_account",
        "SELECT login, password, email FROM account WHERE login = $1",
        { pg::VARCHAR }});

    // Insert a user
    std::string user = "alice", pass = "secret123", email = "alice@example.com";
    bool ok = db.execSync("insert_account", user, pass, email);
    std::cout << "Insert: " << (ok ? "ok" : "failed") << "\n";

    // Query back
    using Row = std::tuple<std::string, std::string, std::string>;
    auto [found, rows] = db.querySync<Row>("find_account", user);

    for (auto& [login, pw, mail] : rows)
        std::cout << login << " / " << mail << "\n";

    db.shutdown();
}
```

## API Overview

All prepared statement APIs take the statement name and `std::string` parameters.

### Synchronous (blocking)

```cpp
// INSERT / UPDATE / DELETE
bool ok = db.execSync("insert_account", login, password, email);

// SELECT
using Row = std::tuple<std::string, std::string>;
auto [ok, rows] = db.querySync<Row>("find_account", login);
```

### Future-based

```cpp
// Fire the query, do other work, then collect
auto f = db.execAsync("insert_account", login, password, email);
// ... do something else ...
auto result = f.get();  // std::optional<bool>

using Row = std::tuple<std::string, int>;
auto f2 = db.queryAsync<Row>("get_player_stats", login);
auto [ok, rows] = f2.get();
```

### Callback-based

Callback runs directly on the DB worker thread — no blocking, no extra threads.

```cpp
db.exec("delete_account", [](std::optional<bool> ok) {
    if (ok && *ok)
        std::cout << "Account deleted\n";
}, login, password);
```

### C++20 Coroutines

```cpp
#include <pgpp_coroutines.h>

// FireAndForget starts immediately, self-destructs on completion
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

### Raw SQL

For schema setup, migrations, or any non-prepared query:

```cpp
db.execRawSync("CREATE INDEX IF NOT EXISTS idx_account_email ON account(email)");

// Async variant
auto f = db.execRawAsync("VACUUM ANALYZE account");
```

### Transactions

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

## Type Mapping

| PostgreSQL | C++ type | OID constant |
|---|---|---|
| VARCHAR, TEXT | `std::string` | `pg::VARCHAR`, `pg::TEXT` |
| INTEGER | `int` | `pg::INT4` |
| BIGINT | `int64_t` | `pg::INT8` |
| SMALLINT | `uint32_t` | `pg::INT2` |
| REAL | `float` | `pg::FLOAT4` |
| DOUBLE PRECISION | `double` | `pg::FLOAT8` |
| BOOLEAN | `bool` | — |

NULL values are left at their C++ default (empty string, 0, false).

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

## Requirements

- C++20
- PostgreSQL (libpq)
- CMake 3.16+

## License

GPL v3 — see [LICENSE](LICENSE).
