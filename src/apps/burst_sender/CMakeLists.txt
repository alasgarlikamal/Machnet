set(target_name burst_sender)
add_executable(${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram gflags)
