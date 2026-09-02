file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

file(GLOB PROTO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${PROTO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_executable(server ${SERVER_SOURCES} ${PROTO_SOURCES})

target_link_libraries(server PRIVATE krpc_core)
