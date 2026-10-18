# Cross-compile Gladius for 64-bit ARM Linux with the host Clang installation.
# Provide a target root filesystem via GLADIUS_ARM64_SYSROOT or the matching
# CMake cache variable. Install the target GCC cross toolchain on the host so
# Clang can use its headers, libraries, and linker support.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED GLADIUS_ARM64_SYSROOT OR GLADIUS_ARM64_SYSROOT STREQUAL "")
    set(GLADIUS_ARM64_SYSROOT "$ENV{GLADIUS_ARM64_SYSROOT}" CACHE PATH
        "Path to the ARM64 Linux target sysroot")
endif()

if(NOT GLADIUS_ARM64_SYSROOT OR NOT IS_DIRECTORY "${GLADIUS_ARM64_SYSROOT}")
    message(FATAL_ERROR
        "Set GLADIUS_ARM64_SYSROOT to an existing ARM64 sysroot before configuring.")
endif()

set(CMAKE_SYSROOT "${GLADIUS_ARM64_SYSROOT}")
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

# Debian's crossbuild-essential-arm64 installs the target GCC toolchain beneath
# /usr. Override this cache entry if the cross toolchain is installed elsewhere.
set(GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT "/usr" CACHE PATH
    "Root containing the AArch64 GCC cross toolchain")
set(CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN "${GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT}")
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN "${GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT}")

# Build-time tools run on the x86_64 host; headers, libraries, and package
# configurations must come from the ARM64 sysroot or the vcpkg target triplet.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
