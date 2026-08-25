cmake_minimum_required(VERSION 3.28)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COMPILER_ENABLE_WARNINGS "Enable common compiler warnings" ON)
option(COMPILER_DEBUG_OUTPUT "Enable compiler debug output" OFF)

file(GLOB_RECURSE COMPILER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(Compiler ${COMPILER_SOURCES})

target_include_directories(Compiler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if (COMPILER_DEBUG_OUTPUT)
    target_compile_definitions(Compiler PRIVATE MY_DEBUG)
endif ()

if (COMPILER_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(Compiler PRIVATE /W4)
    else ()
        target_compile_options(Compiler PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()

if (NOT MSVC)
    # The official judger allows 30 seconds for a clean two-job Release build.
    target_compile_options(Compiler PRIVATE $<$<CONFIG:Release>:-O1>)
endif ()
