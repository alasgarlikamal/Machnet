set(target_name size_sweep)
add_executable(${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram gflags)
