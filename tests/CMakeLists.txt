add_executable(krpc_tests ${CMAKE_CURRENT_SOURCE_DIR}/krpc_test.cc)
target_link_libraries(krpc_tests PRIVATE krpc_core)
add_test(NAME krpc_tests COMMAND krpc_tests)
set_tests_properties(krpc_tests PROPERTIES TIMEOUT 10)
