cmake_minimum_required(VERSION 3.9.0)

# allows changing the msvc runtime library i think?
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0079 NEW)

project(z3dk)

# here because the DLL test needs to know its value
option(ASAR_USE_SANITIZER "Build Asar with ASan and UBSan" OFF)

add_subdirectory(z3asm)

add_subdirectory(z3dk_core)

add_subdirectory(z3lsp)

add_subdirectory(z3disasm)

add_subdirectory(../tests/asar_cpp tests/asar_cpp)

if(TARGET z3asm AND TARGET z3dk-core)
	target_link_libraries(z3asm PRIVATE z3dk-core)
endif()
