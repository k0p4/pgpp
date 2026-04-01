include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Unit tests (no database needed) ──────────────────────────────────────────
add_executable(pgpp_unit_tests
    unit/test_conversions.cpp
    unit/test_connection_string.cpp
    unit/test_pool_state.cpp
    unit/test_structs.cpp
)

target_link_libraries(pgpp_unit_tests PRIVATE pgpp GTest::gtest_main)
target_compile_definitions(pgpp_unit_tests PRIVATE PGPP_TESTING)
set_property(TARGET pgpp_unit_tests PROPERTY CXX_STANDARD 20)

add_test(NAME pgpp_unit_tests COMMAND pgpp_unit_tests)

# ── Integration tests (require PostgreSQL) ───────────────────────────────────
add_executable(pgpp_integration_tests
    integration/main.cpp
    integration/test_connection.cpp
    integration/test_statements.cpp
    integration/test_execution.cpp
    integration/test_pool.cpp
    integration/test_transactions.cpp
    integration/test_coroutines.cpp
    integration/test_raw.cpp
)

target_link_libraries(pgpp_integration_tests PRIVATE pgpp GTest::gtest)
target_compile_definitions(pgpp_integration_tests PRIVATE PGPP_TESTING)
set_property(TARGET pgpp_integration_tests PROPERTY CXX_STANDARD 20)

add_test(NAME pgpp_integration_tests COMMAND pgpp_integration_tests)
