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

### Basic Usage

```cpp
#include <pgpp.h>

PgppConnectionInfo info;
info.host     = "127.0.0.1";
info.port     = 5432;
info.dbname   = "mydb";
info.user     = "postgres";
info.password = "secret";

PgppPool pool;
pool.initialize(info, 4);  // 4 connections

// Prepare a statement
Statement stmt;
stmt.statementName = "get_user";
stmt.statement     = "SELECT login, email FROM users WHERE login = $1";
stmt.variables     = { pg::VARCHAR };
pool.prepareStatement(stmt);

// Synchronous query
auto [ok, rows] = pool.querySync<std::tuple<std::string, std::string>>(
    "get_user", std::string("alice"));

if (ok) {
    for (auto& [login, email] : rows)
        std::cout << login << " — " << email << "\n";
}

pool.shutdown();
```

## API

All query APIs use prepared statement names. Parameters must be `std::string`.

### Synchronous (blocking)

```cpp
bool ok = pool.execSync("insert_user", login, password, email);

auto [ok, rows] = pool.querySync<std::tuple<std::string, int>>(
    "get_scores", username);
```

### Future-based

```cpp
auto future = pool.execAsync("insert_user", login, password, email);
// ... do other work ...
auto result = future.get();  // std::optional<bool>

auto future = pool.queryAsync<std::tuple<std::string, int>>("get_scores", username);
auto [ok, rows] = future.get();
```

### Callback-based

Callback runs on a worker thread — no blocking, no extra threads.

```cpp
pool.exec("insert_user", [](std::optional<bool> ok) {
    if (ok && *ok) std::cout << "inserted\n";
}, login, password, email);

pool.query<std::tuple<std::string, int>>("get_scores",
    [](std::optional<bool> ok, std::vector<std::tuple<std::string, int>> rows) {
        for (auto& [name, score] : rows)
            std::cout << name << ": " << score << "\n";
    }, username);
```

### C++20 Coroutines

```cpp
#include <pgpp_coroutines.h>

FireAndForget doWork(PgppPool& pool) {
    // Execute (INSERT/UPDATE/DELETE)
    auto ok = co_await coExec(pool, "insert_user", login, pw, email);

    // Query (SELECT)
    auto [ok, rows] = co_await coQuery<std::tuple<std::string, std::string>>(
        pool, "get_user", login);
}
```

### Raw SQL

For DDL, migrations, or any non-prepared query:

```cpp
pool.execRawSync("CREATE TABLE IF NOT EXISTS users (login TEXT PRIMARY KEY, email TEXT)");

auto future = pool.execRawAsync("DROP TABLE IF EXISTS temp_data");
```

### Transactions

Auto-commits on success, auto-rollbacks on exception:

```cpp
auto future = pool.transaction([&](PgppConnection& conn) {
    conn.execRaw("UPDATE accounts SET balance = balance - 100 WHERE id = '1'");
    conn.execRaw("UPDATE accounts SET balance = balance + 100 WHERE id = '2'");
});

auto result = future.get();  // false if rolled back
```

## Type Mapping

Query results are mapped to C++ types via tuple:

| PostgreSQL | C++ type | OID constant |
|---|---|---|
| VARCHAR, TEXT | `std::string` | `pg::VARCHAR`, `pg::TEXT` |
| INTEGER | `int` | `pg::INT4` |
| BIGINT | `int64_t` | `pg::INT8` |
| SMALLINT | `uint32_t` | `pg::INT2` |
| REAL | `float` | `pg::FLOAT4` |
| DOUBLE | `double` | `pg::FLOAT8` |
| BOOLEAN | `bool` | — |

NULL values are left at default (empty string, 0, false).

## Pool Statistics

```cpp
pool.totalConnections();  // pool size
pool.freeConnections();   // idle workers
pool.busyConnections();   // active queries
pool.queuedRequests();    // waiting in queue
```

## Logging

By default, pgpp compiles with no logging (zero overhead). If your project provides the [alog](https://github.com/ihor-drachuk/alog) target, logging is enabled automatically:

```cmake
# pgpp detects the alog target and enables logging
FetchContent_MakeAvailable(alog)
FetchContent_MakeAvailable(pgpp)
```

For simple stderr output without alog, define `PGPP_USE_STDERR`:

```cmake
target_compile_definitions(pgpp PRIVATE PGPP_USE_STDERR)
```

## Requirements

- C++20
- PostgreSQL (libpq)
- CMake 3.16+

## License

GPL v3 — see [LICENSE](LICENSE).
