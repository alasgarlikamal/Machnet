# NOTE: Compile flags are now centrally managed in the main CMakeLists.txt
# This ensures consistency across all targets and better performance optimization

# Include 'libdpdk'.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDPDK_STATIC libdpdk>=23.11 libdpdk<24.0 REQUIRED IMPORTED_TARGET)

include_directories(ext)
add_subdirectory(ext)

# Check if DPDK is defined, since it's needed for core/. Else build only non-core
# non-DPDK parts.
if(LIBDPDK_STATIC_FOUND)
  include_directories(include)
  add_subdirectory(core)
  add_subdirectory(benchmark)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(apps)
  add_subdirectory(tools)
else()
  message(WARNING "DPDK not found and RTE_SDK is not set. Building only msg_gen.")
  add_subdirectory(apps/msg_gen)
endif()
