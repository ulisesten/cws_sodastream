cmake_minimum_required(VERSION 3.16)
project(cws_sodastream C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Pull cws as a subdirectory (it ships its own CMakeLists that exports `cws`).
add_subdirectory(third_party/cws)

# GOST cryptography library (usada por core/jwt y core/authorization).
set(GOST_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GOST_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/c_gost_encryption)

find_package(ODBC REQUIRED)

# Gather all .c under src/ and core/ recursively.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c")

add_executable(cws_sodastream ${APP_SOURCES})
target_link_libraries(cws_sodastream PRIVATE cws ODBC::ODBC gost::gost)

# Generador de tablas hash para cfg_getenv (scripts/gen_env_hashes.sh).
add_executable(hashgen EXCLUDE_FROM_ALL scripts/hashgen.c core/hash_table.c)
target_include_directories(hashgen PRIVATE core)
target_include_directories(cws_sodastream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/core)
set_target_properties(cws_sodastream PROPERTIES
    OUTPUT_NAME cws_sodastream
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})