file(GLOB_RECURSE COMPILER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

set(COMPILER_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
list(REMOVE_ITEM COMPILER_SOURCES "${COMPILER_MAIN_SOURCE}")

add_library(CompilerCore STATIC ${COMPILER_SOURCES})
add_executable(${PROJECT_NAME} "${COMPILER_MAIN_SOURCE}")
target_link_libraries(${PROJECT_NAME} PRIVATE CompilerCore)

function(configure_compiler_target TARGET_NAME)
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    set_target_properties(${TARGET_NAME}
            PROPERTIES
            CXX_EXTENSIONS OFF
    )

    if (COMPILER_DEBUG_OUTPUT)
        target_compile_definitions(${TARGET_NAME} PRIVATE MY_DEBUG)
    endif ()

    target_include_directories(${TARGET_NAME}
            PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if (COMPILER_ENABLE_WARNINGS)
        if (MSVC)
            target_compile_options(${TARGET_NAME} PRIVATE /W4)
        else ()
            target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic)
        endif ()
    endif ()
endfunction()

configure_compiler_target(CompilerCore)
configure_compiler_target(${PROJECT_NAME})
