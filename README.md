# pgpp

![CI](../../actions/workflows/ci.yml/badge.svg)

Convenient C++23 wrapper over libpq with connection pooling, prepared statements, and multiple async APIs.

## Features

- **Standard pool design** — a free-list of connections handed out as RAII leases, plus a small executor for the async APIs; the sync API runs on your thread with no hop
- **Multiple APIs** — sync, future, callback, C++20 coroutines
- **Auto-reconnect** — dead connections are restored transparently
- **Prepared statements** — registered once, available on all connections
- **Transactions** — auto-rollback on a failed statement, `return false` or exception
- **Clean connections** — a stray transaction is rolled back on return; optional full session reset
- **Shutdown that does not wait** — in-flight statements are cancelled, queued requests resolve
- **Raw SQL** — for migrations, DDL, one-off queries
- **Optional logging** — zero-overhead no-op by default, [alog](https://github.com/ihor-drachuk/alog) integration available
- **C++23**, no exceptions, binary result decoding, depends only on libpq

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
db.initialize({.dbname = "mydb", .host = "127.0.0.1", .user = "postgres", .password = passwordFromVault()}, 4);

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

// Transactions: ROLLBACK and false if a statement fails, work returns false, or throws
auto committed = db.transaction([](PgppConnection& conn) {
    return conn.execRaw("UPDATE wallet SET balance = balance - 100 WHERE id = '1'")
        && conn.execRaw("UPDATE wallet SET balance = balance + 100 WHERE id = '2'");
}).get();   // std::optional<bool>: nullopt only when the pool is shutting down
```

See the [Usage Guide](docs/usage.md) for detailed API reference, coroutine examples, transactions, logging configuration, and more.

## Type Mapping

Results are fetched in PostgreSQL's binary format and decoded directly; nothing is parsed from text.

| PostgreSQL column | C++ | OID |
|---|---|---|
| TEXT, VARCHAR, CHAR(n), NAME, JSON, JSONB, XML, string literals | `std::string` | `pg::TEXT`, `pg::VARCHAR`, ... |
| SMALLINT, INTEGER, BIGINT, OID | any integral type (`int`, `int16_t`, `int64_t`, `uint32_t`, ...), range-checked | `pg::INT2`, `pg::INT4`, `pg::INT8`, `pg::OID` |
| REAL, DOUBLE PRECISION, and the integer types | `float` / `double` | `pg::FLOAT4` / `pg::FLOAT8` |
| BOOLEAN | `bool` | `pg::BOOL` |

A column of any other type must be cast in SQL: `::text` to read it as `std::string`
(timestamps, uuid, ...), `::int8` or `::float8` for `numeric`. A value that does not fit the
C++ type (`COUNT(*)` above `INT_MAX` into `int`, 40000 into `int16_t`) fails the query instead
of wrapping. NULL is left at the C++ default (empty string, 0, false), or read into a
`std::optional<T>` column as `nullopt`; a `std::optional<std::string>` parameter that is empty
is sent as NULL.

## Requirements

- C++23 compiler with `std::expected` (GCC 12+, Clang 16+/libc++ or 19+/libstdc++, MSVC 2022 17.3+)
- PostgreSQL (libpq)
- CMake 3.20+ (3.21+ for the development presets)

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, testing setup, and project structure.

## License

GPL v3 — see [LICENSE](LICENSE).
