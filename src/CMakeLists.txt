file(GLOB KRPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
list(FILTER KRPC_SOURCES EXCLUDE REGEX "\\.pb\\.cc$")

krpc_add_protobuf_library(krpc_header_proto "${CMAKE_CURRENT_SOURCE_DIR}/Krpcheader.proto")
krpc_sources_depend_on_proto(krpc_header_proto ${KRPC_SOURCES})

add_library(krpc_core STATIC ${KRPC_SOURCES})

target_include_directories(krpc_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(krpc_core
    SYSTEM PUBLIC
        "${MUDUO_INCLUDE_DIR}"
        "${ZOOKEEPER_INCLUDE_DIR}"
        "${GLOG_INCLUDE_DIR}")

target_compile_definitions(krpc_core PUBLIC THREADED)

target_link_libraries(krpc_core
    PUBLIC
        krpc_project_options
        krpc_header_proto
        ${KRPC_DEPENDENCIES})
