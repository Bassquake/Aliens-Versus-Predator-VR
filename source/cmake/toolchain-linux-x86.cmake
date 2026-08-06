# 32-bit x86 Linux build, using the native gcc with -m32 (gcc-multilib), not a
# separate i686 cross compiler — that is how Ubuntu/Debian ship 32-bit support.
#
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install gcc-multilib g++-multilib libgl-dev:i386
#
# Usage:
#   cmake -S source -B build-x86 -DAVP_ENABLE_LINUX=ON \
#         -DCMAKE_TOOLCHAIN_FILE=source/cmake/toolchain-linux-x86.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER   gcc)
set(CMAKE_CXX_COMPILER g++)

set(CMAKE_C_FLAGS_INIT   "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")

# Make find_package/find_library look in the i386 multiarch dir first, otherwise
# FindOpenGL happily returns the host's 64-bit /usr/lib/x86_64-linux-gnu/libGL.so and
# the link dies with "skipping incompatible ... when searching for -lGL".
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/lib/i386-linux-gnu /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# pkg-config (used for the system-FFmpeg fallback) needs pointing at the i386 .pc files.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
