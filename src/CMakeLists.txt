file(GLOB_RECURSE COMPILER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(${PROJECT_NAME} ${COMPILER_SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
set_target_properties(${PROJECT_NAME}
        PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "Compiler"
)

if (COMPILER_DEBUG_OUTPUT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MY_DEBUG)
endif ()

target_include_directories(${PROJECT_NAME}
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

if (COMPILER_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4)
    else ()
        target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
