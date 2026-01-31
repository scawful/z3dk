cmake_minimum_required(VERSION 3.9.0)

add_executable(z3disasm 
  main.cc
  utils.cc
  options.cc
  symbols.cc
  hooks.cc
  formatter.cc
)

target_compile_features(z3disasm PRIVATE cxx_std_20)

target_include_directories(z3disasm PRIVATE
  "../z3dk_core"
  "../third_party"
)

target_link_libraries(z3disasm PRIVATE z3dk-core)

install(TARGETS z3disasm RUNTIME DESTINATION bin)
