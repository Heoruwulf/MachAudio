cmake_minimum_required(VERSION 3.15)
project(MachAudio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Security and Quality Flags
set(COMMON_FLAGS "-Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -mavx2 -mfma")
add_definitions(-D_GNU_SOURCE -DLOG_DEBUG)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")

option(BUILD_FUZZERS "Build libFuzzer targets (requires Clang)" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address,undefined -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

if(BUILD_FUZZERS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=fuzzer-no-link")
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_search_module(LIBURING REQUIRED liburing)
pkg_search_module(LIBUV REQUIRED libuv)
pkg_check_modules(OPUS REQUIRED opus)


# Unity Test Framework
include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.5.2
)
FetchContent_MakeAvailable(unity)

# Include directories
include_directories(
    include
    ${LIBURING_INCLUDE_DIRS}
    ${LIBUV_INCLUDE_DIRS}
    ${OPUS_INCLUDE_DIRS}
)

enable_testing()

# Targets
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_subdirectory(src)
add_subdirectory(tests)
