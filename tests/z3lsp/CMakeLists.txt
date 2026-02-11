
add_executable(z3lsp_utils_test utils_test.cc)
target_link_libraries(z3lsp_utils_test PRIVATE z3lsp-lib)
target_include_directories(z3lsp_utils_test PRIVATE "${CMAKE_SOURCE_DIR}/src/z3lsp" "${CMAKE_SOURCE_DIR}/src/third_party")
target_compile_features(z3lsp_utils_test PRIVATE cxx_std_20)
add_test(NAME z3lsp_utils_test COMMAND z3lsp_utils_test)

add_executable(z3lsp_knowledge_test knowledge_test.cc)
target_link_libraries(z3lsp_knowledge_test PRIVATE z3lsp-lib)
target_include_directories(z3lsp_knowledge_test PRIVATE "${CMAKE_SOURCE_DIR}/src/z3lsp" "${CMAKE_SOURCE_DIR}/src/third_party" "${CMAKE_SOURCE_DIR}/src/z3dk_core")
target_compile_features(z3lsp_knowledge_test PRIVATE cxx_std_20)
add_test(NAME z3lsp_knowledge_test COMMAND z3lsp_knowledge_test)
