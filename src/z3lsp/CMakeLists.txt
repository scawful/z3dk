cmake_minimum_required(VERSION 3.13)

add_executable(z3lsp
	main.cc
	logging.cc
	utils.cc
	state.cc
	project_graph.cc
	lsp_transport.cc
	mesen_client.cc
	parser.cc
)

target_link_libraries(z3lsp PRIVATE z3dk-core)

target_compile_features(z3lsp PRIVATE cxx_std_17)

target_include_directories(z3lsp PRIVATE "../z3dk_core" "../z3asm" "../third_party")

install(TARGETS z3lsp RUNTIME DESTINATION bin)
