cmake_minimum_required(VERSION 3.9.0)

add_library(
  z3dk-core STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/assembler.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/config.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/emit.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/lint.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/opcode_table.cc"
)

target_compile_features(z3dk-core PRIVATE cxx_std_17)

target_include_directories(z3dk-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../z3asm"
)

target_link_libraries(z3dk-core PRIVATE libz3dk-static)

set_target_properties(z3dk-core PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
)
