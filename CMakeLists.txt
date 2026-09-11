cmake_minimum_required(VERSION 3.20)

project(minidb
    VERSION 1.0.0
    DESCRIPTION "Mini sistema gestor de base de datos persistente"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIDB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MINIDB_BUILD_TESTS "Build the unit and integration tests" ON)

# Warnings applied to every target. -Wconversion and -Wsign-conversion are
# deliberately enabled: this codebase does a lot of narrowing arithmetic on page
# offsets (size_t -> uint16_t), and those are exactly the warnings that catch
# slot-directory bugs.
add_library(minidb_warnings INTERFACE)
target_compile_options(minidb_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference)

# Sanitizers live here instead of a dedicated cmake/ module: it is four lines.
add_library(minidb_sanitizers INTERFACE)
if(MINIDB_ENABLE_SANITIZERS)
    target_compile_options(minidb_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(minidb_sanitizers INTERFACE
        -fsanitize=address,undefined)
endif()

add_library(minidb_core
    src/common/serialization.cpp
    src/common/value.cpp)

target_include_directories(minidb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minidb_core PUBLIC minidb_warnings minidb_sanitizers)

if(MINIDB_BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    include(GoogleTest)

    # Each test file becomes its own CTest target so a failure names the
    # component that broke.
    set(MINIDB_TEST_SOURCES
        tests/serialization_test.cpp)

    foreach(test_source IN LISTS MINIDB_TEST_SOURCES)
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(${test_name} ${test_source})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${test_name} PRIVATE minidb_core GTest::gtest_main)
        # PRE_TEST enumerates the cases when ctest runs rather than at build
        # time. The default POST_BUILD mode races with parallel builds and can
        # silently register a stale, partial list of tests, which would hide
        # failures.
        gtest_discover_tests(${test_name} DISCOVERY_MODE PRE_TEST)
    endforeach()
endif()
