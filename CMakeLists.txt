cmake_minimum_required(VERSION 3.15)  # Increased for better performance features
project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)

# =============================================================================
# CENTRALIZED COMPILE CONFIGURATION
# =============================================================================

# Set minimum CMake version and policies for modern CMake features
# if(POLICY CMP0091)
#     cmake_policy(SET CMP0091 NEW)  # MSVC runtime library
# endif()

# Set C and C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compile commands export for better tooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# PERFORMANCE-OPTIMIZED COMPILE FLAGS
# =============================================================================

# Base flags for all configurations (conservative flags for compatibility)
set(MACHNET_C_FLAGS_BASE "-Wall -Wextra")
set(MACHNET_CXX_FLAGS_BASE "-Wall -Wextra")

# Performance optimization flags (shared between C and C++)
set(MACHNET_PERF_FLAGS_BASE "-march=native -mtune=native -ffast-math -funroll-loops")

# MACHNET-specific performance flags (applied to MACHNET targets only)
set(MACHNET_SPECIFIC_CXX_FLAGS "-fno-rtti;-fno-exceptions")

# Release configuration - maximum performance
set(MACHNET_C_FLAGS_RELEASE "${MACHNET_C_FLAGS_BASE} ${MACHNET_PERF_FLAGS_BASE} -O3 -DNDEBUG -flto")
set(MACHNET_CXX_FLAGS_RELEASE "${MACHNET_CXX_FLAGS_BASE} ${MACHNET_PERF_FLAGS_BASE} -O3 -DNDEBUG -Wno-unused-value -flto")

# Debug configuration - debugging and sanitizers
set(MACHNET_C_FLAGS_DEBUG "${MACHNET_C_FLAGS_BASE} -O0 -g -DDEBUG -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined")
set(MACHNET_CXX_FLAGS_DEBUG "${MACHNET_CXX_FLAGS_BASE} -O0 -g -DDEBUG -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined")

# RelWithDebInfo configuration - optimized with debug info
set(MACHNET_C_FLAGS_RELWITHDEBINFO "${MACHNET_C_FLAGS_BASE} ${MACHNET_PERF_FLAGS_BASE} -O3 -DNDEBUG -g")
set(MACHNET_CXX_FLAGS_RELWITHDEBINFO "${MACHNET_CXX_FLAGS_BASE} ${MACHNET_PERF_FLAGS_BASE} -O3 -DNDEBUG -g -Wno-unused-value")

# Apply conservative flags globally (third-party libraries need RTTI)
set(CMAKE_C_FLAGS "${MACHNET_C_FLAGS_BASE}")
set(CMAKE_C_FLAGS_RELEASE "${MACHNET_C_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_DEBUG "${MACHNET_C_FLAGS_DEBUG}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${MACHNET_C_FLAGS_RELWITHDEBINFO}")

set(CMAKE_CXX_FLAGS "${MACHNET_CXX_FLAGS_BASE}")
set(CMAKE_CXX_FLAGS_RELEASE "${MACHNET_CXX_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_DEBUG "${MACHNET_CXX_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${MACHNET_CXX_FLAGS_RELWITHDEBINFO}")

# Linker flags for debug configuration (sanitizers)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined")
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined")

# =============================================================================
# CENTRALIZED MACRO DEFINITIONS
# =============================================================================

# Function to apply MACHNET debug definitions to targets
function(target_apply_machnet_debug_definitions target_name)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${target_name} PRIVATE MACHNET_DEBUG)
    endif()
endfunction()

# Function to apply MACHNET-specific performance optimizations
function(target_apply_machnet_performance_flags target_name)
    # Apply no-RTTI and no-exceptions for MACHNET targets only
    target_compile_options(${target_name} PRIVATE ${MACHNET_SPECIFIC_CXX_FLAGS})
endfunction()

# Function to apply common MACHNET definitions to targets
function(target_apply_machnet_definitions target_name)
    target_compile_definitions(${target_name} PRIVATE MACHNET)
    target_apply_machnet_debug_definitions(${target_name})
    target_apply_machnet_performance_flags(${target_name})
endfunction()

# =============================================================================
# BUILD CONFIGURATION INFO
# =============================================================================

message(STATUS "=== MACHNET Build Configuration ===")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "CXX Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE}}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}")
message(STATUS "===================================")

# =============================================================================
# THIRD PARTY DEPENDENCIES
# =============================================================================

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules
# SET(CMAKE_FIND_LIBRARY_SUFFIXES .a .so ${CMAKE_FIND_LIBRARY_SUFFIXES})

find_package(gflags QUIET)
if (NOT gflags_FOUND)
    # SET(BUILD_STATIC_LIBS ON)
    SET(BUILD_gflags_nothreads_LIBS ON)
    SET(BUILD_gflags_LIBS ON)
    add_subdirectory(third_party/gflags)
    include_directories(SYSTEM "${CMAKE_CURRENT_BINARY_DIR}/third_party/gflags/include/")
endif()

# Glog doesn't correctly define its include directories, so we need to specify
# manually
add_subdirectory(third_party/glog)
include_directories(SYSTEM third_party/glog/src)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/glog)

add_subdirectory(third_party/googletest)
include_directories(SYSTEM third_party/googletest/googlemock/include)

# Build google benchmark (target: benchmark)
# do not build tests of benchmarking lib
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
add_subdirectory(third_party/googlebench)
include_directories(SYSTEM third_party/googlebench/include)

# Common sub-projects: HdrHistogram
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")

add_subdirectory(third_party/HdrHistogram_c)
include_directories(SYSTEM third_party/HdrHistogram_c/include/)
include_directories(SYSTEM third_party/xxHash)

set(BUILD_TESTING ON)

add_subdirectory(src)
add_subdirectory(examples/webserver)
add_subdirectory(examples/webclient)
