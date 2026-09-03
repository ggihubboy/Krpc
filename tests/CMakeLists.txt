add_executable(krpc_tests ${CMAKE_CURRENT_SOURCE_DIR}/krpc_test.cc)
target_link_libraries(krpc_tests PRIVATE krpc_core)
add_test(NAME krpc_tests COMMAND krpc_tests)
set_tests_properties(krpc_tests PROPERTIES TIMEOUT 10)

add_test(NAME krpc_script_helpers COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/script_test.sh)
set_tests_properties(krpc_script_helpers PROPERTIES TIMEOUT 30)

add_test(NAME krpc_integration COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration_smoke.sh)
set_tests_properties(krpc_integration PROPERTIES
    TIMEOUT 120
    SKIP_RETURN_CODE 77
    ENVIRONMENT "KRPC_BIN_DIR=${CMAKE_BINARY_DIR}/bin")
