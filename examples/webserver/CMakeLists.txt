set(target_name webserver)
add_executable (${target_name} main.cc mongoose.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram gflags pthread)

# Apply centralized MACHNET compile definitions
target_apply_machnet_definitions(${target_name})

target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/ext ${CMAKE_SOURCE_DIR}/src/include)
