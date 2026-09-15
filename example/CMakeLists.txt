protobuf_generate_cpp(KRPC_EXAMPLE_PROTO_SOURCES KRPC_EXAMPLE_PROTO_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/user.proto")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${KRPC_EXAMPLE_PROTO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(krpc_example_proto STATIC ${KRPC_EXAMPLE_PROTO_SOURCES})
target_include_directories(krpc_example_proto
    PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(krpc_example_proto
    PUBLIC
        krpc_project_options
        protobuf::libprotobuf)

add_subdirectory(callee)
add_subdirectory(caller)
add_subdirectory(bench)