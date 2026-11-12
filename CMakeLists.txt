cmake_minimum_required(VERSION 3.16)
project(linux_internals_lab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LAB_NATIVE_ARCH "Pass -march=native (off for portable CI binaries)" ON)
option(LAB_ASAN "AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LAB_UBSAN "UndefinedBehaviorSanitizer alone" OFF)
option(LAB_TSAN "ThreadSanitizer (cannot combine with ASan)" OFF)

if(LAB_ASAN AND LAB_TSAN)
    message(FATAL_ERROR "LAB_ASAN and LAB_TSAN cannot be combined")
endif()

add_executable(linuxlab
    src/main.cpp
    src/self_test.cpp
    src/exp_process.cpp
    src/exp_thread.cpp
    src/exp_context.cpp
    src/exp_contention.cpp
    src/exp_false_sharing.cpp
    src/exp_memory.cpp
    src/exp_affinity.cpp
    src/exp_syscall.cpp
)

target_include_directories(linuxlab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# -std=c++20 does not imply GNU extensions, and the lab calls
# pthread_setaffinity_np, MAP_ANONYMOUS, madvise advice values, and sched_getcpu.
target_compile_definitions(linuxlab PRIVATE _GNU_SOURCE)

target_compile_options(linuxlab PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow
    -Wpointer-arith -Wcast-qual
)

find_package(Threads REQUIRED)
target_link_libraries(linuxlab PRIVATE Threads::Threads)

if(LAB_NATIVE_ARCH)
    target_compile_options(linuxlab PRIVATE -march=native)
endif()

if(LAB_ASAN)
    target_compile_options(linuxlab PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(linuxlab PRIVATE -fsanitize=address,undefined)
elseif(LAB_UBSAN)
    target_compile_options(linuxlab PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options(linuxlab PRIVATE -fsanitize=undefined)
elseif(LAB_TSAN)
    target_compile_options(linuxlab PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(linuxlab PRIVATE -fsanitize=thread)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Native arch: ${LAB_NATIVE_ARCH}")
