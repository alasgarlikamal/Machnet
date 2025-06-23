cmake_minimum_required(VERSION 3.0)

project(webclient VERSION 0.0.1 DESCRIPTION "High-performance HTTP benchmarking tool" LANGUAGES C)

# Include testing support (disabled for third-party modules)
include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules

# Apply centralized MACHNET definitions (includes MACHNET_DEBUG for debug builds)
# This uses the centralized configuration from the main CMakeLists.txt

# Define source files from the local src directory
set(WEBCLIENT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ae.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ae_epoll.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/aprintf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hdr_histogram.c" # Use the local hdr_histogram.c
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ssl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tinymt64.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/units.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wrk.c" # Entry point
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zmalloc.c"
    "${CMAKE_SOURCE_DIR}/src/ext/machnet.c" # Add machnet.c directly
)

# Define executable
add_executable(webclient ${WEBCLIENT_SOURCES})

# Apply centralized MACHNET compile definitions
target_apply_machnet_debug_definitions(webclient)

# Locate Lua library and include directory
set(LUA_LIB "${CMAKE_CURRENT_SOURCE_DIR}/deps/luajit/src/libluajit.so")
set(LUA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/luajit/src")

# Debugging messages for Lua paths
message(STATUS "Lua library found at: ${LUA_LIB}")
message(STATUS "Lua include directory found at: ${LUA_INCLUDE_DIR}")

# Add a custom command to build and install LuaJIT if libluajit.so is not found
if(NOT EXISTS "${LUA_LIB}")
    message(STATUS "LuaJIT library not found. Building and installing LuaJIT...")
    execute_process(COMMAND make WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/luajit")
    execute_process(COMMAND sudo make install WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/luajit")
    execute_process(COMMAND sudo ldconfig)
endif()

# Link libraries
# Add other dependencies as needed (e.g., machnet_shim, gflags, glog, ssl, crypto)
target_link_libraries(webclient PRIVATE gflags glog ssl crypto ${LUA_LIB} machnet_shim pthread m)

# Include directories
target_include_directories(webclient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"          # Source directory for webclient
    "${CMAKE_SOURCE_DIR}/src/include"          # For pause.h
    "${CMAKE_SOURCE_DIR}/src/ext"              # For machnet.h and related headers
    "${LUA_INCLUDE_DIR}"                       # Lua headers
    "${CMAKE_SOURCE_DIR}/third_party/gflags/include"
    "${CMAKE_SOURCE_DIR}/third_party/glog/src"
)

# Informational message
message(STATUS "webclient target successfully created.")
