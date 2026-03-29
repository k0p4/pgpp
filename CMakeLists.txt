cmake_minimum_required(VERSION 3.16)
project(pgpp LANGUAGES CXX)

set(PostgreSQL_ADDITIONAL_VERSIONS "15")
find_package(PostgreSQL REQUIRED)

add_library(pgpp STATIC
    include/pgpp/pgpp.h
    include/pgpp/pgpp_connection.h
    include/pgpp/pgpp_coroutines.h
    src/pgpp_log.h
    src/pgpp.cpp
    src/pgpp_connection.cpp
)

set_property(TARGET pgpp PROPERTY CXX_STANDARD 20)

target_include_directories(pgpp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pgpp PUBLIC PostgreSQL::PostgreSQL)

if(TARGET alog)
    target_link_libraries(pgpp PRIVATE alog)
    target_compile_definitions(pgpp PRIVATE PGPP_USE_ALOG)
endif()
