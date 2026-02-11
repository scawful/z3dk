cmake_minimum_required(VERSION 3.13)

add_library(z3lsp-lib STATIC
	logging.cc
	utils.cc
	state.cc
	project_graph.cc
	lsp_transport.cc
	mesen_client.cc
	parser.cc
	knowledge.cc
)

target_link_libraries(z3lsp-lib PUBLIC z3dk-core)
target_include_directories(z3lsp-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "../z3dk_core" "../z3asm" "../third_party")
target_compile_features(z3lsp-lib PUBLIC cxx_std_20)

add_executable(z3lsp main.cc)
target_link_libraries(z3lsp PRIVATE z3lsp-lib)

install(TARGETS z3lsp RUNTIME DESTINATION bin)
