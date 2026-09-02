add_executable(krpc_tests ${CMAKE_CURRENT_SOURCE_DIR}/krpc_test.cc)
target_link_libraries(krpc_tests krpc_core ${LIBS})
target_include_directories(krpc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/include)
target_compile_options(krpc_tests PRIVATE -Wall)
set_target_properties(krpc_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
add_test(NAME krpc_tests COMMAND ${PROJECT_SOURCE_DIR}/bin/krpc_tests)
