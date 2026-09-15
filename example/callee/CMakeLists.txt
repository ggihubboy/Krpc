file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
krpc_sources_depend_on_proto(krpc_example_proto ${SERVER_SOURCES})

add_executable(server ${SERVER_SOURCES})

target_link_libraries(server PRIVATE krpc_core krpc_example_proto)
