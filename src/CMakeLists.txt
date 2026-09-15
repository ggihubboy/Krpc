file(GLOB KRPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
list(FILTER KRPC_SOURCES EXCLUDE REGEX "\\.pb\\.cc$")

set(KRPC_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${KRPC_PROTO_GEN_DIR}")
if(NOT Protobuf_PROTOC_EXECUTABLE)
    set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC_EXECUTABLE}")
endif()
add_custom_command(
    OUTPUT
        "${KRPC_PROTO_GEN_DIR}/Krpcheader.pb.cc"
        "${KRPC_PROTO_GEN_DIR}/Krpcheader.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            --cpp_out=${KRPC_PROTO_GEN_DIR}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/Krpcheader.proto
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Krpcheader.proto"
    VERBATIM)

set(KRPC_PROTO_SOURCES "${KRPC_PROTO_GEN_DIR}/Krpcheader.pb.cc")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${KRPC_PROTO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(krpc_core STATIC ${KRPC_SOURCES} ${KRPC_PROTO_SOURCES})

target_include_directories(krpc_core
    PUBLIC
        "${KRPC_PROTO_GEN_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(krpc_core
    SYSTEM PUBLIC
        "${MUDUO_INCLUDE_DIR}"
        "${ZOOKEEPER_INCLUDE_DIR}"
        "${GLOG_INCLUDE_DIR}")

target_link_libraries(krpc_core
    PUBLIC
        krpc_project_options
        ${KRPC_DEPENDENCIES})