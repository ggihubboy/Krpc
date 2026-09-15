file(GLOB CLIENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_executable(client ${CLIENT_SOURCES})

target_link_libraries(client PRIVATE krpc_core krpc_example_proto)
