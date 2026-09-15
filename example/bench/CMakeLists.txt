file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
krpc_sources_depend_on_proto(krpc_example_proto ${BENCH_SOURCES})

add_executable(krpc_bench ${BENCH_SOURCES})
target_link_libraries(krpc_bench PRIVATE krpc_core krpc_example_proto)
