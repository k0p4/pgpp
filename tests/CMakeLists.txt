include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(COMMON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ── Unit tests (no database needed) ──────────────────────────────────────────
file(GLOB_RECURSE UNIT_TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)

add_executable(pgpp_unit_tests ${UNIT_TEST_SOURCES})
target_link_libraries(pgpp_unit_tests PRIVATE pgpp GTest::gtest_main)
target_compile_definitions(pgpp_unit_tests PRIVATE PGPP_TESTING)
set_property(TARGET pgpp_unit_tests PROPERTY CXX_STANDARD 20)

add_test(NAME pgpp_unit_tests COMMAND pgpp_unit_tests)

# ── Integration tests (require PostgreSQL) ───────────────────────────────────
file(GLOB_RECURSE INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS integration/*.cpp)

add_executable(pgpp_integration_tests ${INTEGRATION_TEST_SOURCES})
target_include_directories(pgpp_integration_tests PRIVATE ${COMMON_INCLUDE_DIR})
target_link_libraries(pgpp_integration_tests PRIVATE pgpp GTest::gtest)
target_compile_definitions(pgpp_integration_tests PRIVATE PGPP_TESTING)
set_property(TARGET pgpp_integration_tests PROPERTY CXX_STANDARD 20)

add_test(NAME pgpp_integration_tests COMMAND pgpp_integration_tests)
