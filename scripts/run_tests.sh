#!/bin/bash
# pgpp test runner
#
# Usage:
#   ./scripts/run_tests.sh              # Run all tests (unit + integration)
#   ./scripts/run_tests.sh unit         # Run only unit tests (no DB needed)
#   ./scripts/run_tests.sh integration  # Run only integration tests (needs PostgreSQL)
#
# Environment variables for integration tests:
#   PGPP_TEST_DBNAME   (default: draft)
#   PGPP_TEST_HOST     (default: 127.0.0.1)
#   PGPP_TEST_USER     (default: postgres)
#   PGPP_TEST_PASSWORD (default: 123)
#   PGPP_TEST_PORT     (default: 5432)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

MODE="${1:-all}"

echo "=========================================="
echo "  pgpp Test Runner"
echo "=========================================="
echo ""

# Build if needed
if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

echo "Building tests..."
cd "$BUILD_DIR"
cmake "$PROJECT_ROOT" -DPGPP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
cmake --build . --parallel 2>&1 | tail -10
echo ""

UNIT_EXIT=0
INTEG_EXIT=0

# ── Unit tests ───────────────────────────────────────────────────────────────

if [ "$MODE" = "all" ] || [ "$MODE" = "unit" ]; then
    echo "=========================================="
    echo "  UNIT TESTS (no database needed)"
    echo "=========================================="
    echo ""

    if [ -f "$BUILD_DIR/tests/pgpp_unit_tests" ]; then
        "$BUILD_DIR/tests/pgpp_unit_tests" --gtest_color=yes
        UNIT_EXIT=$?
    else
        echo "ERROR: pgpp_unit_tests binary not found"
        UNIT_EXIT=1
    fi
    echo ""
fi

# ── Integration tests ────────────────────────────────────────────────────────

if [ "$MODE" = "all" ] || [ "$MODE" = "integration" ]; then
    echo "=========================================="
    echo "  INTEGRATION TESTS (PostgreSQL required)"
    echo "=========================================="
    echo ""
    echo "  Database : ${PGPP_TEST_DBNAME:-draft}"
    echo "  Host     : ${PGPP_TEST_HOST:-127.0.0.1}"
    echo "  Port     : ${PGPP_TEST_PORT:-5432}"
    echo "  User     : ${PGPP_TEST_USER:-postgres}"
    echo ""

    # Quick connectivity check
    if command -v pg_isready >/dev/null 2>&1; then
        if ! pg_isready -h "${PGPP_TEST_HOST:-127.0.0.1}" -p "${PGPP_TEST_PORT:-5432}" -q 2>/dev/null; then
            echo "WARNING: PostgreSQL does not appear to be running at ${PGPP_TEST_HOST:-127.0.0.1}:${PGPP_TEST_PORT:-5432}"
            echo "Integration tests will likely fail."
            echo ""
        fi
    fi

    if [ -f "$BUILD_DIR/tests/pgpp_integration_tests" ]; then
        "$BUILD_DIR/tests/pgpp_integration_tests" --gtest_color=yes
        INTEG_EXIT=$?
    else
        echo "ERROR: pgpp_integration_tests binary not found"
        INTEG_EXIT=1
    fi
    echo ""
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo "=========================================="
echo "  SUMMARY"
echo "=========================================="

if [ "$MODE" = "all" ] || [ "$MODE" = "unit" ]; then
    if [ $UNIT_EXIT -eq 0 ]; then
        echo "  Unit tests:        PASSED"
    else
        echo "  Unit tests:        FAILED (exit $UNIT_EXIT)"
    fi
fi

if [ "$MODE" = "all" ] || [ "$MODE" = "integration" ]; then
    if [ $INTEG_EXIT -eq 0 ]; then
        echo "  Integration tests: PASSED"
    else
        echo "  Integration tests: FAILED (exit $INTEG_EXIT)"
    fi
fi

echo "=========================================="

EXIT_CODE=$((UNIT_EXIT + INTEG_EXIT))
exit $EXIT_CODE
