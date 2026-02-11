cmake_minimum_required(VERSION 3.9.0)

# allows changing the msvc runtime library i think?
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0079 NEW)

project(z3dk)

# here because the DLL test needs to know its value
option(ASAR_USE_SANITIZER "Build Asar with ASan and UBSan" OFF)
option(Z3DK_BUILD_LSP "Build z3lsp (requires C++20)" ON)

include(CheckCXXCompilerFlag)
set(Z3DK_HAS_CXX20 ON)
if(Z3DK_BUILD_LSP)
	if(MSVC)
		check_cxx_compiler_flag("/std:c++20" Z3DK_HAS_CXX20)
	else()
		check_cxx_compiler_flag("-std=c++20" Z3DK_HAS_CXX20)
	endif()
endif()

add_subdirectory(z3asm)

add_subdirectory(z3dk_core)

if(Z3DK_BUILD_LSP)
	if(NOT Z3DK_HAS_CXX20)
		message(WARNING "z3lsp requires C++20; disabling (set Z3DK_BUILD_LSP=OFF to silence).")
	else()
		add_subdirectory(z3lsp)
		add_subdirectory(../tests/z3lsp tests/z3lsp)
	endif()
endif()

add_subdirectory(z3disasm)

add_subdirectory(../tests/asar_cpp tests/asar_cpp)

if(TARGET z3asm AND TARGET z3dk-core)
	target_link_libraries(z3asm PRIVATE z3dk-core)
endif()

