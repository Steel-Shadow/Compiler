cmake_minimum_required(VERSION 3.15)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Compiler LANGUAGES C CXX)

option(COMPILER_ENABLE_WARNINGS "Enable common compiler warnings" ON)
option(COMPILER_DEBUG_OUTPUT "Enable compiler debug output" OFF)

add_subdirectory(src)
