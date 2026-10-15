# Cross-compiles for 64-bit Windows with llvm-mingw (https://github.com/mstorsjo/llvm-mingw), unpacked anywhere; no
# install needed. Point LLVM_MINGW (environment) at its folder, and SDL2's MinGW development package at SDL2_DIR:
#   LLVM_MINGW=/path/to/llvm-mingw cmake -DCMAKE_TOOLCHAIN_FILE=CMake/platforms/llvm-mingw64.toolchain.cmake \
#     -DSDL2_DIR=/path/to/SDL2-2.x/x86_64-w64-mingw32/lib/cmake/SDL2 ...
# Packaging/fp/package-windows.sh does all of it.
set(MINGW_CROSS TRUE)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{LLVM_MINGW})
  message(FATAL_ERROR "Set LLVM_MINGW to llvm-mingw's folder")
endif()
set(LLVM_MINGW "$ENV{LLVM_MINGW}")
set(_triplet x86_64-w64-mingw32)

set(CMAKE_C_COMPILER "${LLVM_MINGW}/bin/${_triplet}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/${_triplet}-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW}/bin/${_triplet}-windres")
set(CMAKE_STRIP "${LLVM_MINGW}/bin/${_triplet}-strip")
set(CMAKE_AR "${LLVM_MINGW}/bin/${_triplet}-ar")
set(CMAKE_RANLIB "${LLVM_MINGW}/bin/${_triplet}-ranlib")

# The C++ runtime (libc++, libunwind) and winpthreads linked in, so the game needs no DLLs of the toolchain's
# (Windows' own libraries are import libraries, still loaded from the system).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/${_triplet}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(SYSTEM_BITS x64)
