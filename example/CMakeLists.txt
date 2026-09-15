krpc_add_protobuf_library(krpc_example_proto "${CMAKE_CURRENT_SOURCE_DIR}/user.proto")

add_subdirectory(callee)
add_subdirectory(caller)
add_subdirectory(bench)
