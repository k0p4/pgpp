# pgpp

![CI](../../actions/workflows/ci.yml/badge.svg)

Convenient C++20 wrapper over libpq with connection pooling, prepared statements, and multiple async APIs.

## Features

- **Thread-per-connection pool** — each worker owns its connection exclusively, no sharing
- **Multiple APIs** — sync, future, callback, C++20 coroutines
- **Auto-reconnect** — dead connections are restored transparently
- **Prepared statements** — registered once, available on all connections
- **Transactions** — auto-rollback on exception
- **Raw SQL** — for migrations, DDL, one-off queries
- **Optional logging** — zero-overhead no-op by default, [alog](https://github.com/ihor-drachuk/alog) integration available
- **C++20**, depends only on libpq

## Integration

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

## Quick Example

```cpp
#include <pgpp/pgpp.h>

PgppPool db;
db.initialize({.host = "127.0.0.1", .dbname = "mydb", .user = "postgres", .password = "secret"}, 4);

db.prepareStatement({"insert_user",
    "INSERT INTO users (name, email) VALUES ($1, $2)",
    {pg::VARCHAR, pg::VARCHAR}});

db.prepareStatement({"find_user",
    "SELECT name, email FROM users WHERE name = $1",
    {pg::VARCHAR}});

// Synchronous
std::string name = "alice", email = "alice@example.com";
db.execSync("insert_user", name, email);

// Query with typed rows
using Row = std::tuple<std::string, std::string>;
auto [ok, rows] = db.querySync<Row>("find_user", name);

// Future-based
auto future = db.execAsync("insert_user", name, email);
future.get();  // std::optional<bool>

// Coroutines (C++20)
auto [ok2, rows2] = co_await coQuery<Row>(db, "find_user", name);

// Transactions (auto-rollback on exception)
db.transaction([](PgppConnection& conn) {
    conn.execRaw("UPDATE wallet SET balance = balance - 100 WHERE id = '1'");
    conn.execRaw("UPDATE wallet SET balance = balance + 100 WHERE id = '2'");
}).get();
```

See the [Usage Guide](docs/usage.md) for detailed API reference, coroutine examples, transactions, logging configuration, and more.

## Type Mapping

| PostgreSQL | C++ | OID |
|---|---|---|
| VARCHAR, TEXT | `std::string` | `pg::VARCHAR`, `pg::TEXT` |
| INTEGER | `int` | `pg::INT4` |
| BIGINT | `int64_t` | `pg::INT8` |
| SMALLINT | `uint32_t` | `pg::INT2` |
| REAL / DOUBLE | `float` / `double` | `pg::FLOAT4` / `pg::FLOAT8` |
| BOOLEAN | `bool` | — |

NULL values are left at their C++ default (empty string, 0, false).

## Requirements

- C++20 compiler
- PostgreSQL (libpq)
- CMake 3.16+

## License

GPL v3 — see [LICENSE](LICENSE).
