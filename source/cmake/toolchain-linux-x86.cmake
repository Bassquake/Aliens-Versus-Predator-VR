# 32-bit x86 Linux build. Prefers the i686 CROSS compiler, and falls back to the
# native gcc with -m32 (gcc-multilib) when that is not installed.
#
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install gcc-i686-linux-gnu g++-i686-linux-gnu libgl-dev:i386
#
# The cross compiler is preferred because on Ubuntu gcc-multilib CANNOT be installed
# alongside gcc-aarch64-linux-gnu (apt reports "held broken packages"), so a machine
# using multilib can build x86 or arm64 but never both. The i686 and aarch64 cross
# compilers coexist.
#
# Usage:
#   cmake -S source -B build-x86 -DAVP_ENABLE_LINUX=ON \
#         -DCMAKE_TOOLCHAIN_FILE=source/cmake/toolchain-linux-x86.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

find_program(AVP_I686_GCC i686-linux-gnu-gcc)
find_program(AVP_I686_GXX i686-linux-gnu-g++)
if(AVP_I686_GCC AND AVP_I686_GXX)
    set(CMAKE_C_COMPILER   ${AVP_I686_GCC})
    set(CMAKE_CXX_COMPILER ${AVP_I686_GXX})
else()
    set(CMAKE_C_COMPILER   gcc)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_C_FLAGS_INIT   "-m32")
    set(CMAKE_CXX_FLAGS_INIT "-m32")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
endif()

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
