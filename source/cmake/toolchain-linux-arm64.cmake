# aarch64 (arm64) Linux cross build.
#
#   sudo dpkg --add-architecture arm64 && sudo apt update
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libgl-dev:arm64
#
# Usage:
#   cmake -S source -B build-arm64 -DAVP_ENABLE_LINUX=ON \
#         -DCMAKE_TOOLCHAIN_FILE=source/cmake/toolchain-linux-arm64.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# On Ubuntu the arm64 libs come from multiarch (/usr/lib/aarch64-linux-gnu), not from a
# self-contained sysroot, so point the search at that dir rather than setting
# CMAKE_SYSROOT. Without CMAKE_LIBRARY_ARCHITECTURE, FindOpenGL returns the host's
# x86-64 libGL.so and the link fails with "skipping incompatible".
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/lib/aarch64-linux-gnu /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
