cmake_minimum_required(VERSION 3.16)
project(pgpp LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(PostgreSQL_ADDITIONAL_VERSIONS "15")
find_package(PostgreSQL REQUIRED)

add_library(pgpp STATIC
    pgpp.cpp
    pgpp.h
    pgpp_connection.cpp
    pgpp_connection.h
    pgpp_coroutines.h
    pgpp_log.h
)

set_property(TARGET pgpp PROPERTY CXX_STANDARD 20)
target_link_libraries(pgpp PUBLIC PostgreSQL::PostgreSQL)
target_include_directories(pgpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(TARGET alog)
    target_link_libraries(pgpp PRIVATE alog)
    target_compile_definitions(pgpp PRIVATE PGPP_USE_ALOG)
endif()

if(DEBUG_BUILD)
    target_compile_definitions(pgpp PUBLIC DEBUG_BUILD)
endif()
