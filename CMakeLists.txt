cmake_minimum_required(VERSION 3.20)
project(pgpp LANGUAGES CXX)

set(PostgreSQL_ADDITIONAL_VERSIONS "17" "16" "15" "14" "13")
find_package(PostgreSQL REQUIRED)
find_package(Threads REQUIRED)

add_library(pgpp STATIC
    include/pgpp/pgpp.h
    include/pgpp/pgpp_connection.h
    include/pgpp/pgpp_coroutines.h
    src/pgpp_log.h
    src/pgpp.cpp
    src/pgpp_connection.cpp
)
add_library(pgpp::pgpp ALIAS pgpp)

# PUBLIC: the headers use std::expected, so every consumer of the target
# (FetchContent included) inherits C++23 instead of having to set it by hand.
target_compile_features(pgpp PUBLIC cxx_std_23)

target_include_directories(pgpp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Threads::Threads is PUBLIC: the executor uses std::thread, and the headers
# use std::future / std::mutex, so consumers need the threading library too.
target_link_libraries(pgpp PUBLIC PostgreSQL::PostgreSQL Threads::Threads)

# Warnings for pgpp's own sources and tests only; nothing here reaches consumers.
if(MSVC)
    set(PGPP_WARNING_OPTIONS /W4)
    set(PGPP_WARNINGS_AS_ERRORS_OPTION /WX)
else()
    set(PGPP_WARNING_OPTIONS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    set(PGPP_WARNINGS_AS_ERRORS_OPTION -Werror)
endif()
option(PGPP_WARNINGS_AS_ERRORS "Treat warnings in pgpp's own sources and tests as errors" OFF)
if(PGPP_WARNINGS_AS_ERRORS)
    list(APPEND PGPP_WARNING_OPTIONS ${PGPP_WARNINGS_AS_ERRORS_OPTION})
endif()
target_compile_options(pgpp PRIVATE ${PGPP_WARNING_OPTIONS})

if(TARGET alog)
    target_link_libraries(pgpp PRIVATE alog)
    target_compile_definitions(pgpp PRIVATE PGPP_USE_ALOG)
endif()

option(PGPP_BUILD_TESTS "Build pgpp tests" OFF)
if(PGPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
