cmake_minimum_required(VERSION 3.13)

project(Z3DK)

# When using Homebrew LLVM/Clang on macOS, link against its libc++ to avoid
# ABI mismatches (e.g. missing std::__1::__hash_memory with system libc++).
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  get_filename_component(_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_LLVM_PREFIX "${_COMPILER_DIR}" DIRECTORY)
  set(_LIBCXX_DIR "${_LLVM_PREFIX}/lib/c++")
  if(EXISTS "${_LIBCXX_DIR}/libc++.dylib")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -L${_LIBCXX_DIR} -Wl,-rpath,${_LIBCXX_DIR}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -L${_LIBCXX_DIR} -Wl,-rpath,${_LIBCXX_DIR}")
  endif()
endif()

# Global Output Directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src)
