cmake_minimum_required(VERSION 3.16)
project(cws_sodastream C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Pull cws as a subdirectory (it ships its own CMakeLists that exports `cws`).
add_subdirectory(third_party/cws)

find_package(ODBC REQUIRED)

# Gather all .c under src/ and core/ recursively.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c")

add_executable(cws_sodastream ${APP_SOURCES})
target_link_libraries(cws_sodastream PRIVATE cws ODBC::ODBC)
target_include_directories(cws_sodastream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/core)
set_target_properties(cws_sodastream PROPERTIES
    OUTPUT_NAME cws_sodastream
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})