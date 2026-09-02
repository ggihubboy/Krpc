file(GLOB KRPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
file(GLOB KRPC_PROTO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.pb.cc")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${KRPC_PROTO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(krpc_core STATIC ${KRPC_SOURCES})

target_include_directories(krpc_core
    PUBLIC
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