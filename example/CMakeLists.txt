if(NOT Protobuf_PROTOC_EXECUTABLE)
    set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC_EXECUTABLE}")
endif()
set(KRPC_EXAMPLE_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${KRPC_EXAMPLE_PROTO_GEN_DIR}")
add_custom_command(
    OUTPUT
        "${KRPC_EXAMPLE_PROTO_GEN_DIR}/user.pb.cc"
        "${KRPC_EXAMPLE_PROTO_GEN_DIR}/user.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            --cpp_out=${KRPC_EXAMPLE_PROTO_GEN_DIR}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/user.proto
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user.proto"
    VERBATIM)

set(KRPC_EXAMPLE_PROTO_SOURCES "${KRPC_EXAMPLE_PROTO_GEN_DIR}/user.pb.cc")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${KRPC_EXAMPLE_PROTO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(krpc_example_proto STATIC ${KRPC_EXAMPLE_PROTO_SOURCES})
target_include_directories(krpc_example_proto
    PUBLIC
        "${KRPC_EXAMPLE_PROTO_GEN_DIR}")
target_link_libraries(krpc_example_proto
    PUBLIC
        krpc_project_options
        protobuf::libprotobuf)

add_subdirectory(callee)
add_subdirectory(caller)
add_subdirectory(bench)