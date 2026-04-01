# Contributing to pgpp

## Prerequisites

| Tool | Notes |
|------|-------|
| C++20 compiler | MSVC 2022, GCC 11+, Clang 14+ |
| CMake 3.16+ | |
| vcpkg | Set `VCPKG_ROOT` environment variable |
| Python 3.6+ | Optional — for the build script |
| Docker | Required for integration tests only |

### vcpkg setup

If you don't have vcpkg yet:

```bash
# Linux / macOS
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg  # add to your shell profile
```

```bat
:: Windows
git clone https://github.com/microsoft/vcpkg.git C:\Tools\vcpkg
C:\Tools\vcpkg\bootstrap-vcpkg.bat
:: Set VCPKG_ROOT as a system/user environment variable
```

### Platform-specific dependencies

**Linux (Debian/Ubuntu):**

```bash
sudo apt-get install build-essential cmake pkg-config
```

**macOS:**

```bash
xcode-select --install
brew install cmake
```

**Windows:**

Install Visual Studio 2022 with the "Desktop development with C++" workload.

## Building

### Build script (recommended)

The `build_and_test.py` script handles the full pipeline — prerequisite checks, configure, build, and tests:

```bash
python build_and_test.py                        # full pipeline (configure + build + all tests)
python build_and_test.py --preset dev-release   # release build
python build_and_test.py --unit-only            # skip integration tests (no Docker needed)
python build_and_test.py --skip-tests           # only configure and build
python build_and_test.py --clean                # clean rebuild
python build_and_test.py --yes                  # skip confirmation pauses (CI mode)
python build_and_test.py --help                 # show help and available presets
```

The script checks for cmake, vcpkg, and Docker before starting. If a non-critical tool is missing (e.g. Docker), it warns and continues with what's available.

### CMake directly

```bash
cmake --preset dev-debug         # configure (installs libpq via vcpkg)
cmake --build --preset dev-debug # build
ctest --preset dev-debug         # run all tests
```

Replace `dev-debug` with `dev-release` for an optimized build.

Build outputs go to `build/dev-debug/` or `build/dev-release/`.

## Testing

### Unit tests

Unit tests validate type conversions, connection string building, pool state machine, and struct invariants. They do **not** require a database.

```bash
ctest --preset dev-debug
```

Or run the executable directly:

```bash
# Linux / macOS
./build/dev-debug/tests/pgpp_unit_tests

# Windows
build\dev-debug\tests\Debug\pgpp_unit_tests.exe
```

### Integration tests

Integration tests cover CRUD operations, pool concurrency, transactions, coroutines, and raw SQL. They require a running PostgreSQL instance.

#### Automatic (Docker)

The integration test executable manages its own PostgreSQL container via Docker CLI. Just make sure Docker is running:

```bash
# Linux / macOS
./build/dev-debug/tests/pgpp_integration_tests

# Windows
build\dev-debug\tests\Debug\pgpp_integration_tests.exe
```

This will:
1. Start a `postgres:16-alpine` container named `pgpp-test-pg-7f3a` on port **15432**
2. Wait for PostgreSQL to accept connections
3. Run all integration tests
4. Stop and remove the container

#### Manual PostgreSQL

If you prefer to use your own PostgreSQL instance, set `PGPP_SKIP_DOCKER=1` and configure the connection via environment variables:

```bash
export PGPP_SKIP_DOCKER=1
export PGPP_TEST_HOST=127.0.0.1
export PGPP_TEST_PORT=5432
export PGPP_TEST_USER=postgres
export PGPP_TEST_PASSWORD=yourpassword
export PGPP_TEST_DBNAME=pgpp_test
```

All variables have defaults (see `tests/common/test_config.h`), so you only need to override what differs from your setup.

## Project layout

```
pgpp/
  include/pgpp/         Public headers (pgpp.h, pgpp_connection.h, pgpp_coroutines.h)
  src/                  Implementation files
  tests/
    unit/               Unit tests (no database)
    integration/        Integration tests (require PostgreSQL)
    common/             Shared test utilities (config, Docker fixture)
  docs/                 Specification, usage guide, testing roadmap
  CMakePresets.json     Dev presets (vcpkg + tests enabled)
  vcpkg.json            vcpkg manifest (libpq dependency)
```

## How it fits together

When you consume pgpp via **FetchContent** in another project, `vcpkg.json` and `CMakePresets.json` are ignored entirely — your project provides `PostgreSQL::PostgreSQL` through whatever means it uses (vcpkg, system packages, etc.).

The vcpkg manifest and presets exist solely for **standalone development**: they ensure libpq is available when building pgpp on its own.
