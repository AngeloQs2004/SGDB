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

