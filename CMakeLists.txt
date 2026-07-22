cmake_minimum_required(VERSION 3.16)
project(cws_example_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Pull cws as a subdirectory (it ships its own CMakeLists that exports `cws`).
add_subdirectory(third_party/cws)

add_executable(cws_example_app main.c)
target_link_libraries(cws_example_app PRIVATE cws)
set_target_properties(cws_example_app PROPERTIES
    OUTPUT_NAME cws_example_app
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
