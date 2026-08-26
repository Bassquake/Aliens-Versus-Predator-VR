# Aliens vs Predator - https://github.com/atsb/NakedAVP
CMAKE_MINIMUM_REQUIRED(VERSION 3.22.1)

# default to Release
IF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    MESSAGE(STATUS "No build type specified, defaulting to Release")
    SET(CMAKE_BUILD_TYPE Release CACHE STRING "Build type; one of: Debug Release RelWithDebInfo MinSizeRel" FORCE)
    SET_PROPERTY(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)

# ---------------------------------------------------------------------------
# Linux desktop target (OPT-IN — disabled by default)
#
# SDL3 comes from the bundled extern/SDL3/lib/linux/<arch>/libSDL3.so (x64 / x86 /
# arm64), falling back to a system SDL3 via FindSDL3.cmake if that arch isn't
# bundled. FFmpeg (pkg-config) and OpenAL/OpenGL (find_package) still come from
# system dev packages. This path is kept OFF by default; enable it with:
#   cmake -DAVP_ENABLE_LINUX=ON ...
# ---------------------------------------------------------------------------
option(AVP_ENABLE_LINUX "Build the Linux desktop target (needs system SDL3/FFmpeg/OpenAL/GL dev libs)" OFF)
if (UNIX AND NOT APPLE AND NOT ANDROID)
    set(AVP_LINUX_PLATFORM TRUE)
endif()
if (AVP_LINUX_PLATFORM AND AVP_ENABLE_LINUX)
    set(AVP_LINUX TRUE)
    message(STATUS "AVP: Linux desktop target ENABLED.")
elseif (AVP_LINUX_PLATFORM)
    message(STATUS "AVP: Linux desktop target is present but DISABLED. Re-run with "
                   "-DAVP_ENABLE_LINUX=ON once SDL3/FFmpeg/OpenAL/GL dev libs are installed.")
endif()

# ---------------------------------------------------------------------------
# macOS desktop target (OPT-IN — disabled by default)
#
# Mirrors the Linux target: SDL3, OpenAL and FFmpeg each come from
# extern/<dep>/lib/mac/<arch>/*.dylib when that arch is populated, falling back
# independently to a system copy (FindSDL3.cmake / FindOpenAL / pkg-config) when
# it is not. Only mac/x64 ships today, and all three are bundled there, so it
# needs nothing from Homebrew. Enable with:
#   cmake -DAVP_ENABLE_MAC=ON ...
# ---------------------------------------------------------------------------
option(AVP_ENABLE_MAC "Build the macOS desktop target (SDL3/OpenAL/FFmpeg are bundled for x64)" OFF)
if (APPLE AND NOT IOS AND NOT ANDROID)
    set(AVP_MAC_PLATFORM TRUE)
endif()
if (AVP_MAC_PLATFORM AND AVP_ENABLE_MAC)
    set(AVP_MAC TRUE)
    message(STATUS "AVP: macOS desktop target ENABLED.")
elseif (AVP_MAC_PLATFORM)
    message(STATUS "AVP: macOS desktop target is present but DISABLED. Re-run with "
                   "-DAVP_ENABLE_MAC=ON once SDL3 is installed (brew install sdl3).")
endif()

# The bundled dylibs set the floor: OpenAL Soft is built for macOS 11.3 and FFmpeg
# for 10.13, so 11.3 is the lowest the app as a whole can honestly claim. Has to be
# set BEFORE project() — after it, the compiler checks have already baked in the
# SDK default and this only affects later targets.
if (AVP_MAC AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.3" CACHE STRING "Minimum macOS version" FORCE)
    message(STATUS "AVP: CMAKE_OSX_DEPLOYMENT_TARGET -> 11.3 (floor set by the bundled OpenAL)")
endif()

# ---------------------------------------------------------------------------
# Windows PCVR target (OPT-IN — disabled by default)
#
# Builds the OpenXR VR path (the same one the Quest uses) against desktop GL +
# WGL, running on SteamVR or any desktop OpenXR runtime. Needs the Khronos
# OpenXR loader DLL at extern/OpenXR/lib/windows/<arch>/openxr_loader.dll —
# the exe loads it at runtime (no import lib), and falls back to the flat
# desktop render path when the DLL or an active runtime is absent. The VR exe
# is named avp_<arch>vr.exe and lands in the same build/windows/<arch> folder
# as the flat build so they share assets and DLLs. Enable with:
#   cmake -DAVP_ENABLE_PCVR=ON ...   (use a separate binary dir, e.g.
#   platform/windows/x64vr, so the flat and VR builds can coexist)
# ---------------------------------------------------------------------------
option(AVP_ENABLE_PCVR "Build the Windows OpenXR (SteamVR) PCVR target" OFF)

if (ANDROID)
    PROJECT(avpvr)
endif()
if (WIN32)
    PROJECT(avp)
endif()
if (AVP_LINUX)
    PROJECT(avp)
endif()
if (AVP_MAC)
    PROJECT(avp)
endif()

# required dependencies
if (ANDROID)
    INCLUDE(FindOpenGLES2.cmake)
endif()
INCLUDE(FindSDL3.cmake)
INCLUDE(FindOpenGL)

if (WIN32 OR AVP_LINUX OR AVP_MAC)
    # Single source of truth for which extern/<lib>/lib/<os>/<arch> folder this build
    # uses: x64, x86 or arm64. ARM64/aarch64 needs an explicit test because it is
    # 64-bit, so CMAKE_SIZEOF_VOID_P alone would misidentify it as x64.
    # MSVC_C_ARCHITECTURE_ID is "ARM64" for the MSVC ARM64 toolchain; the generator
    # and system vars cover the Ninja / clang-cl / native-Linux cases.
    #
    # macOS is read from CMAKE_OSX_ARCHITECTURES INSTEAD OF, not in addition to, those
    # vars: it is how you cross-build x86_64 on an Apple Silicon Mac, and there
    # CMAKE_SYSTEM_PROCESSOR still says arm64. Folding both into one string would let
    # that stale arm64 win the regex and silently pick the wrong lib folder.
    if (AVP_MAC AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES AVP_OSX_ARCH_COUNT)
        if (AVP_OSX_ARCH_COUNT GREATER 1)
            message(FATAL_ERROR
                "macOS build: CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'. The "
                "bundled extern/*/lib/mac/<arch> dylibs are single-architecture, so a "
                "universal binary cannot link against them. Configure one arch at a time "
                "and lipo the results afterwards if you need a fat binary.")
        endif()
        set(AVP_ARCH_RAW "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(AVP_ARCH_RAW "${MSVC_C_ARCHITECTURE_ID}/${CMAKE_GENERATOR_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    string(TOLOWER "${AVP_ARCH_RAW}" AVP_ARCH_ID)
    if (AVP_ARCH_ID MATCHES "arm64|aarch64|armv8")
        set(ARCH_DIR "arm64")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ARCH_DIR "x64")
    else()
        set(ARCH_DIR "x86")
    endif()

    if (WIN32)
        set(AVP_LIB_OS "windows")
        message(STATUS "AVP: Windows target architecture -> ${ARCH_DIR}")
    elseif (AVP_MAC)
        set(AVP_LIB_OS "mac")
        message(STATUS "AVP: macOS target architecture -> ${ARCH_DIR}")
    else()
        set(AVP_LIB_OS "linux")
        message(STATUS "AVP: Linux target architecture -> ${ARCH_DIR}")
    endif()

    # Shared-library suffix for the two paths that load bundled libs by filename.
    # (Windows uses import libs and never consults this.)
    if (AVP_MAC)
        set(AVP_SHLIB_SUFFIX ".dylib")
    else()
        set(AVP_SHLIB_SUFFIX ".so")
    endif()
endif()

if (WIN32)
    set(OPENAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/OpenAL32/include" CACHE FILEPATH "" FORCE)
    set(OPENAL_LIBRARY "${CMAKE_SOURCE_DIR}/extern/OpenAL32/lib/windows/${ARCH_DIR}/OpenAL32.lib" CACHE FILEPATH "" FORCE)
endif()

# Linux/macOS: prefer the bundled extern/OpenAL32/lib/<os>/<arch>/libopenal.{so,dylib},
# exactly as we do for SDL3. Falls through to FindOpenAL (system libopenal, or the macOS
# OpenAL.framework) when this arch isn't bundled, so a distro/Homebrew build still works.
if (AVP_LINUX OR AVP_MAC)
    set(AVP_OPENAL_BUNDLED
        "${CMAKE_SOURCE_DIR}/extern/OpenAL32/lib/${AVP_LIB_OS}/${ARCH_DIR}/libopenal${AVP_SHLIB_SUFFIX}")
    if (EXISTS "${AVP_OPENAL_BUNDLED}")
        message(STATUS "AVP: using bundled OpenAL -> ${AVP_OPENAL_BUNDLED}")
        set(OPENAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/OpenAL32/include" CACHE PATH "" FORCE)
        set(OPENAL_LIBRARY "${AVP_OPENAL_BUNDLED}" CACHE FILEPATH "" FORCE)
        set(OPENAL_FOUND TRUE)
        set(AVP_OPENAL_IS_BUNDLED TRUE)
    endif()
endif()

if (NOT ANDROID AND NOT AVP_OPENAL_IS_BUNDLED)
    include(FindOpenAL)
elseif (ANDROID)
    # On Android, we either skip it or point to a local prebuilt
    set(OPENAL_FOUND TRUE)
    set(OPENAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/OpenAL32/include") # Leave empty to avoid the "NOTFOUND" error
    set(lib_path "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}")
    set(OPENAL_LIBRARY "${lib_path}/libopenal.so" CACHE FILEPATH "" FORCE)     # Leave empty; link it later if you add the lib
endif()

if (ANDROID)
    # Tell CMake where to find the SDL3 Android headers and binaries
    # Usually you'd use add_subdirectory(extern/SDL3) here
    add_definitions(-DANDROID)
    # AVP_XR gates the shared OpenXR/VR code (present on Android AND the Windows
    # PCVR build). On Android it is always defined, so every former
    # `#ifdef __ANDROID__` XR guard preprocesses exactly as before; genuinely
    # Android-only code (JNI, EGL, dlopen, gamepads) still uses __ANDROID__.
    add_definitions(-DAVP_XR)
    # Non-VR "phone" build variant: the Gradle `phone` product flavor passes
    # -DAVP_DISABLE_XR=ON, which makes main.c skip OpenXR init entirely and use the
    # flat (windowed) render path + SDL_GL_SwapWindow. The Quest `quest` flavor leaves
    # it unset, so the VR path is unchanged.
    if (AVP_DISABLE_XR)
        add_definitions(-DAVP_DISABLE_XR)
        message(STATUS "AVP: OpenXR disabled — building non-VR phone variant")
    endif()
    # Android uses a specific log library for console output
    find_library(log-lib log)
    set(lib_path "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}")
    set(SDL3_LIBRARY "${lib_path}/libSDL3.so" CACHE FILEPATH "" FORCE)
    set(SDL3_INCLUDE "${CMAKE_SOURCE_DIR}/extern" CACHE PATH "" FORCE)
    set(SDL3_FOUND TRUE)
    # OpenXR Library
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

    add_library(openxr_loader SHARED IMPORTED)
    set_target_properties(openxr_loader PROPERTIES
        IMPORTED_LOCATION
        "${lib_path}/libopenxr_loader.so"
    )

    # Ensure the OpenXR headers are in your include path
    # (Assuming they are in your 'extern' folder like SDL3)
    include_directories("${CMAKE_SOURCE_DIR}/extern")
    #
elseif (AVP_LINUX OR AVP_MAC)
    # Linux/macOS desktop: prefer the bundled shared libs in extern/SDL3/lib/<os>/<arch>
    # (x64 / x86 / arm64), matching the extern/SDL3/lib/windows/<arch> layout. The
    # headers in extern/SDL3 are shared by every platform, so SDL3_INCLUDE points at
    # extern/ (game code includes <SDL3/SDL.h>).
    # If no bundled lib exists for this arch we fall back to the system SDL3 that
    # FindSDL3.cmake located above — never to the Windows import lib. On macOS that
    # fallback also drops the "-framework Cocoa" FindSDL3.cmake appends, which is
    # correct: the bundled libSDL3.dylib links Cocoa itself.
    # ARCH_DIR was resolved once, above.
    set(AVP_SDL3_BUNDLED
        "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/${AVP_LIB_OS}/${ARCH_DIR}/libSDL3${AVP_SHLIB_SUFFIX}")
    if (EXISTS "${AVP_SDL3_BUNDLED}")
        message(STATUS "AVP: using bundled SDL3 -> ${AVP_SDL3_BUNDLED}")
        set(SDL3_LIBRARY "${AVP_SDL3_BUNDLED}" CACHE FILEPATH "" FORCE)
        set(SDL3_INCLUDE "${CMAKE_SOURCE_DIR}/extern" CACHE PATH "" FORCE)
        set(SDL3_FOUND TRUE)
    elseif (SDL3_FOUND)
        message(STATUS "AVP: no bundled SDL3 for ${AVP_LIB_OS}/${ARCH_DIR}; using system SDL3 -> ${SDL3_LIBRARY}")
    else()
        message(FATAL_ERROR "No bundled SDL3 at ${AVP_SDL3_BUNDLED} and no system SDL3 "
                            "found. Install SDL3 (>=3.5) dev files (brew install sdl3 on "
                            "macOS) or point CMake at your build via $SDL3DIR / "
                            "CMAKE_PREFIX_PATH.")
    endif()
else()
    # Windows: ARCH_DIR was resolved once, above.
    set(SDL3_LIBRARY "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/windows/${ARCH_DIR}/SDL3.lib" CACHE FILEPATH "" FORCE)
    set(SDL3_INCLUDE "${CMAKE_SOURCE_DIR}/extern" CACHE PATH "" FORCE)
    set(SDL3_FOUND TRUE)
endif()

IF(NOT SDL3_FOUND)
    MESSAGE(FATAL_ERROR "SDL 3.0 was requested but CMake couldn't find it.")
ENDIF(NOT SDL3_FOUND)

# The Windows build links against prebuilt import libs under extern/*/lib/windows/
# <arch>. Not every arch is populated (arm64 in particular), so fail here with the
# exact missing file rather than letting it surface as a cryptic LNK1104 later.
if (WIN32)
    set(AVP_REQUIRED_WIN_LIBS
        "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/windows/${ARCH_DIR}/SDL3.lib"
        "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/windows/${ARCH_DIR}/SDL3.dll"
        "${CMAKE_SOURCE_DIR}/extern/OpenAL32/lib/windows/${ARCH_DIR}/OpenAL32.lib")
    foreach (avp_lib avcodec avformat avutil swscale swresample)
        list(APPEND AVP_REQUIRED_WIN_LIBS
             "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/${avp_lib}.lib")
    endforeach()

    set(AVP_MISSING_WIN_LIBS "")
    foreach (avp_lib ${AVP_REQUIRED_WIN_LIBS})
        if (NOT EXISTS "${avp_lib}")
            list(APPEND AVP_MISSING_WIN_LIBS "${avp_lib}")
        endif()
    endforeach()
    if (AVP_MISSING_WIN_LIBS)
        string(REPLACE ";" "\n  " AVP_MISSING_WIN_LIBS_TEXT "${AVP_MISSING_WIN_LIBS}")
        message(FATAL_ERROR
            "Windows ${ARCH_DIR} build: the following bundled libraries are missing:\n"
            "  ${AVP_MISSING_WIN_LIBS_TEXT}\n"
            "Build or drop in ${ARCH_DIR} versions of these, or configure for an arch "
            "that is fully populated (x64 / x86).")
    endif()

    # Same header-vs-library major check the Linux path does, but driven off the
    # versioned DLL name (avcodec-63.dll) since there is no soname to read. Windows was
    # unguarded before, which is exactly how extern/FFmpeg/lib/windows/arm64 ended up a
    # major ahead of the headers without anything complaining.
    foreach (avp_lib avcodec avformat avutil swscale swresample)
        string(TOUPPER "${avp_lib}" _upper)
        set(_hdr "")
        set(_hdr_major "")
        foreach (_vh version_major.h version.h)
            if (NOT _hdr AND EXISTS "${CMAKE_SOURCE_DIR}/extern/FFmpeg/include/lib${avp_lib}/${_vh}")
                file(STRINGS "${CMAKE_SOURCE_DIR}/extern/FFmpeg/include/lib${avp_lib}/${_vh}"
                     _hdr REGEX "define LIB${_upper}_VERSION_MAJOR")
            endif()
        endforeach()
        if (_hdr)
            string(REGEX MATCH "[0-9]+" _hdr_major "${_hdr}")
        endif()

        file(GLOB _dll "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/${avp_lib}-*.dll")
        set(_dll_major "")
        if (_dll)
            list(GET _dll 0 _dll)
            get_filename_component(_dll_name "${_dll}" NAME)
            string(REGEX MATCH "${avp_lib}-([0-9]+)\\.dll" _m "${_dll_name}")
            set(_dll_major "${CMAKE_MATCH_1}")
        endif()

        if (_hdr_major AND _dll_major AND NOT _hdr_major STREQUAL _dll_major)
            message(WARNING
                "FFmpeg version mismatch on ${ARCH_DIR}: extern/FFmpeg/include declares "
                "lib${avp_lib} major ${_hdr_major} but the bundled runtime is "
                "${avp_lib}-${_dll_major}.dll. It will link, then misbehave in the FMV "
                "path. Rebuild the bundled libs to match the headers.")
        endif()
    endforeach()

    # PCVR: the shared VR code compiles in (AVP_XR) with the Win32/WGL OpenXR
    # binding (AVP_PCVR). Only the Khronos loader DLL is needed — main.c loads
    # it with LoadLibraryA at runtime, so there is no import lib to link and a
    # machine without the DLL/runtime still runs the exe flat. Hard-fail here
    # when it is missing, matching the bundled-lib checks above, since the
    # post-build copy step would otherwise fail mid-build instead.
    if (AVP_ENABLE_PCVR)
        set(AVP_OPENXR_DLL "${CMAKE_SOURCE_DIR}/extern/OpenXR/lib/windows/${ARCH_DIR}/openxr_loader.dll")
        if (NOT EXISTS "${AVP_OPENXR_DLL}")
            message(FATAL_ERROR
                "PCVR build: missing ${AVP_OPENXR_DLL}\n"
                "Drop in the Khronos OpenXR loader for ${ARCH_DIR} (openxr_loader.dll "
                "from an OpenXR-SDK release at github.com/KhronosGroup/OpenXR-SDK), "
                "or configure with -DAVP_ENABLE_PCVR=OFF.")
        endif()
        add_definitions(-DAVP_XR -DAVP_PCVR)
        message(STATUS "AVP: PCVR (OpenXR) build enabled -> avp_${ARCH_DIR}vr.exe")
    endif()
endif()
    MESSAGE(WARNING "SDL 3.0 support is STABLE.")

if (ANDROID)
    set(OPENGL_TYPE "OPENGLES2")
    set(OPENGLES_FOUND TRUE)
    # The NDK provides these by default; no find_package needed
    set(OPENGLES_gl_LIBRARY GLESv3)
    set(EGL_LIBRARIES EGL)
else()

    # Use Desktop OpenGL if it is available, else try OpenGL ES.
    IF(NOT OPENGL_TYPE)
        SET(OPENGL_TYPE AUTO CACHE STRING "OpenGL Version; one of: AUTO OPENGL OPENGLES2")
        SET_PROPERTY(CACHE OPENGL_TYPE PROPERTY STRINGS "AUTO" "OPENGL" "OPENGLES2")
    ENDIF(NOT OPENGL_TYPE)

    IF(NOT OPENGL_TYPE STREQUAL "AUTO" AND NOT OPENGL_TYPE STREQUAL "OPENGL" AND NOT OPENGL_TYPE STREQUAL "OPENGLES2")
        MESSAGE(FATAL_ERROR "Invalid OPENGL_TYPE setting ${OPENGL_TYPE}; must be one of AUTO OPENGL OPENGLES2")
    ENDIF(NOT OPENGL_TYPE STREQUAL "AUTO" AND NOT OPENGL_TYPE STREQUAL "OPENGL" AND NOT OPENGL_TYPE STREQUAL "OPENGLES2")

    IF(OPENGL_TYPE STREQUAL "AUTO")
      IF(OPENGL_FOUND)
        MESSAGE(STATUS "OpenGL found; using that.")
        SET(OPENGL_TYPE "OPENGL")
      ENDIF(OPENGL_FOUND)
    ENDIF(OPENGL_TYPE STREQUAL "AUTO")

    IF(OPENGL_TYPE STREQUAL "AUTO")
      IF(OPENGLES_FOUND)
        MESSAGE(STATUS "OpenGL ES 2 found; using that.")
        SET(OPENGL_TYPE "OPENGLES2")
      ENDIF(OPENGLES_FOUND)
    ENDIF(OPENGL_TYPE STREQUAL "AUTO")

    IF(OPENGL_TYPE STREQUAL "AUTO")
      MESSAGE(FATAL_ERROR "OpenGL is required but CMake couldn't find it.")
    ENDIF(OPENGL_TYPE STREQUAL "AUTO")

    IF(OPENGL_TYPE STREQUAL "OPENGL")
      IF(NOT OPENGL_FOUND)
        MESSAGE(FATAL_ERROR "OpenGL was requested but CMake couldn't find it.")
      ENDIF(NOT OPENGL_FOUND)
    ENDIF(OPENGL_TYPE STREQUAL "OPENGL")

    IF(OPENGL_TYPE STREQUAL "OPENGLES2")
      IF(NOT OPENGLES_FOUND)
        MESSAGE(FATAL_ERROR "OpenGL ES was requested but CMake couldn't find it.")
      ENDIF(NOT OPENGLES_FOUND)
    ENDIF(OPENGL_TYPE STREQUAL "OPENGLES2")

    # OpenAL
    IF(NOT OPENAL_FOUND)
        MESSAGE(FATAL_ERROR "OpenAL is required but CMake couldn't find it.")
    ENDIF(NOT OPENAL_FOUND)
endif()

# Windows/Android compile against the bundled FFmpeg headers in extern/FFmpeg/include.
# Linux does too when extern/FFmpeg/lib/linux/<arch> is populated; otherwise it falls
# back to the system FFmpeg dev packages via pkg-config, which bring their own
# matching headers and link flags (so we must NOT force the bundled headers on that
# path — mixing the two is how you get a header/library version mismatch).
# avcodec/avformat/avutil/swscale are the four the engine actually calls; nothing in
# src/ references swr_* or swresample, so it is linked only when present.
set(AVP_FFMPEG_LIBS avcodec avformat avutil swscale)

if (AVP_LINUX OR AVP_MAC)
    find_program(AVP_READELF NAMES readelf llvm-readelf eu-readelf)
    find_program(AVP_OTOOL NAMES otool llvm-otool)

    set(AVP_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/${AVP_LIB_OS}/${ARCH_DIR}")
    set(AVP_FFMPEG_BUNDLED_LIBS "")
    set(AVP_FFMPEG_MISSING "")
    foreach (avp_lib ${AVP_FFMPEG_LIBS})
        if (EXISTS "${AVP_FFMPEG_DIR}/lib${avp_lib}${AVP_SHLIB_SUFFIX}")
            list(APPEND AVP_FFMPEG_BUNDLED_LIBS "${AVP_FFMPEG_DIR}/lib${avp_lib}${AVP_SHLIB_SUFFIX}")
        else()
            list(APPEND AVP_FFMPEG_MISSING "lib${avp_lib}${AVP_SHLIB_SUFFIX}")
        endif()
    endforeach()
    if (EXISTS "${AVP_FFMPEG_DIR}/libswresample${AVP_SHLIB_SUFFIX}")
        list(APPEND AVP_FFMPEG_BUNDLED_LIBS "${AVP_FFMPEG_DIR}/libswresample${AVP_SHLIB_SUFFIX}")
    endif()

    if (AVP_FFMPEG_BUNDLED_LIBS AND NOT AVP_FFMPEG_MISSING)
        set(AVP_FFMPEG_IS_BUNDLED TRUE)
        message(STATUS "AVP: using bundled FFmpeg -> ${AVP_FFMPEG_DIR}")
    elseif (AVP_FFMPEG_BUNDLED_LIBS)
        # A partially-populated folder is the dangerous case: it looks bundled but
        # would silently link some libs from extern/ and the rest from the system.
        string(REPLACE ";" " " AVP_FFMPEG_MISSING_TEXT "${AVP_FFMPEG_MISSING}")
        message(WARNING
            "extern/FFmpeg/lib/${AVP_LIB_OS}/${ARCH_DIR} is incomplete (missing: "
            "${AVP_FFMPEG_MISSING_TEXT}) — falling back to system FFmpeg via "
            "pkg-config and IGNORING the bundled libs entirely.")
    endif()

    # extern/FFmpeg/include is shared by every arch, so a bundled lib whose major
    # soname disagrees with those headers will still LINK (C symbols aren't versioned)
    # and then misread struct fields at runtime, because fmv.c touches AVCodecContext /
    # AVFrame / AVFormatContext directly. Catch that here instead of in a debugger.
    if (AVP_MAC)
        set(AVP_FFMPEG_VERSION_TOOL "${AVP_OTOOL}")
    else()
        set(AVP_FFMPEG_VERSION_TOOL "${AVP_READELF}")
    endif()
    if (AVP_FFMPEG_IS_BUNDLED AND AVP_FFMPEG_VERSION_TOOL)
        foreach (avp_lib ${AVP_FFMPEG_LIBS})
            string(TOUPPER "${avp_lib}" _upper)
            # Newer FFmpeg splits the major out into version_major.h; older trees (and
            # libavutil even in 8.x) keep it in version.h. file(STRINGS) hard-errors on
            # a missing file, so test before reading, and reset per iteration or a
            # failed read silently reuses the previous lib's number.
            set(_hdr "")
            set(_hdr_major "")
            foreach (_vh version_major.h version.h)
                if (NOT _hdr AND EXISTS "${CMAKE_SOURCE_DIR}/extern/FFmpeg/include/lib${avp_lib}/${_vh}")
                    file(STRINGS "${CMAKE_SOURCE_DIR}/extern/FFmpeg/include/lib${avp_lib}/${_vh}"
                         _hdr REGEX "define LIB${_upper}_VERSION_MAJOR")
                endif()
            endforeach()
            if (_hdr)
                string(REGEX MATCH "[0-9]+" _hdr_major "${_hdr}")
            endif()

            # Mach-O has no SONAME; the equivalent is the LC_ID_DYLIB install name,
            # which is what the linker copies into the exe and what dyld then searches
            # the rpath for ("@rpath/libavcodec.62.dylib"). otool -D prints it.
            set(_lib_major "")
            if (AVP_MAC)
                execute_process(COMMAND "${AVP_OTOOL}" -D
                                "${AVP_FFMPEG_DIR}/lib${avp_lib}.dylib"
                                OUTPUT_VARIABLE _dyn ERROR_QUIET)
                string(REGEX MATCH "lib${avp_lib}\\.([0-9]+)\\.dylib" _m "${_dyn}")
                set(_lib_major "${CMAKE_MATCH_1}")
                set(_lib_desc "install name lib${avp_lib}.${_lib_major}.dylib")
            else()
                execute_process(COMMAND "${AVP_READELF}" -d
                                "${AVP_FFMPEG_DIR}/lib${avp_lib}.so"
                                OUTPUT_VARIABLE _dyn ERROR_QUIET)
                string(REGEX MATCH "SONAME[^\n]*\\[lib${avp_lib}\\.so\\.([0-9]+)\\]" _m "${_dyn}")
                set(_lib_major "${CMAKE_MATCH_1}")
                set(_lib_desc "lib${avp_lib}.so soname ${_lib_major}")
            endif()

            if (_hdr_major AND _lib_major AND NOT _hdr_major STREQUAL _lib_major)
                message(WARNING
                    "FFmpeg version mismatch on ${AVP_LIB_OS}/${ARCH_DIR}: extern/FFmpeg/include "
                    "declares lib${avp_lib} major ${_hdr_major} but the bundled lib is "
                    "${_lib_desc}. It will link, then misbehave in the FMV path. Rebuild "
                    "the bundled lib to match the headers.")
            endif()
        endforeach()
    endif()
endif()

if (NOT (AVP_LINUX OR AVP_MAC) OR AVP_FFMPEG_IS_BUNDLED)
    include_directories(
        ${CMAKE_SOURCE_DIR}/extern/FFmpeg/include
    )
endif()
if ((AVP_LINUX OR AVP_MAC) AND NOT AVP_FFMPEG_IS_BUNDLED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
endif()

#add_library(ffmpeg_jni SHARED src/ffmpeg_jni.c)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libavcodec.so"
)

add_library(avdevice SHARED IMPORTED)
set_target_properties(avdevice PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libavdevice.so"
)

add_library(avfilter SHARED IMPORTED)
set_target_properties(avfilter PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libavfilter.so"
)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libavformat.so"
)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libavutil.so"
)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libswresample.so"
)

add_library(swscale SHARED IMPORTED)
set_target_properties(swscale PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/${ANDROID_ABI}/libswscale.so"
)

#target_link_libraries(ffmpeg_jni
#    avcodec
#    avformat
#    avutil
#    swscale
#    swresample
#    log
#)

# required source files
#LIST(APPEND source src/android-bridge.cpp)
LIST(APPEND source src/files.c)
LIST(APPEND source src/winapi.c)
LIST(APPEND source src/stubs.c)
LIST(APPEND source src/version.c)
LIST(APPEND source src/mathline.c)
LIST(APPEND source src/net.c)
LIST(APPEND source src/frustum.c)
LIST(APPEND source src/kshape.c)
LIST(APPEND source src/map.c)
LIST(APPEND source src/maths.c)
LIST(APPEND source src/md5.c)
LIST(APPEND source src/mem3dc.c)
LIST(APPEND source src/mem3dcpp.cpp)
LIST(APPEND source src/module.c)
LIST(APPEND source src/morph.c)
LIST(APPEND source src/object.c)
LIST(APPEND source src/shpanim.c)
LIST(APPEND source src/sphere.c)
LIST(APPEND source src/tables.c)
LIST(APPEND source src/vdb.c)
LIST(APPEND source src/avp/ai_sight.c)
LIST(APPEND source src/avp/avpview.c)
LIST(APPEND source src/avp/bh_agun.c)
LIST(APPEND source src/avp/bh_ais.c)
LIST(APPEND source src/avp/bh_alien.c)
LIST(APPEND source src/avp/bh_binsw.c)
LIST(APPEND source src/avp/bh_cable.c)
LIST(APPEND source src/avp/bh_corpse.c)
LIST(APPEND source src/avp/bh_deathvol.c)
LIST(APPEND source src/avp/bh_debri.c)
LIST(APPEND source src/avp/bh_dummy.c)
LIST(APPEND source src/avp/bh_fan.c)
LIST(APPEND source src/avp/bh_far.c)
LIST(APPEND source src/avp/bh_fhug.c)
LIST(APPEND source src/avp/bh_gener.c)
LIST(APPEND source src/avp/bh_ldoor.c)
LIST(APPEND source src/avp/bh_lift.c)
LIST(APPEND source src/avp/bh_light.c)
LIST(APPEND source src/avp/bh_lnksw.c)
LIST(APPEND source src/avp/bh_ltfx.c)
LIST(APPEND source src/avp/bh_marin.c)
LIST(APPEND source src/avp/bh_mission.c)
LIST(APPEND source src/avp/bh_near.c)
LIST(APPEND source src/avp/bh_pargen.c)
LIST(APPEND source src/avp/bh_plachier.c)
LIST(APPEND source src/avp/bh_plift.c)
LIST(APPEND source src/avp/bh_pred.c)
LIST(APPEND source src/avp/bh_queen.c)
LIST(APPEND source src/avp/bh_rubberduck.c)
LIST(APPEND source src/avp/bh_selfdest.c)
LIST(APPEND source src/avp/bh_snds.c)
LIST(APPEND source src/avp/bh_spcl.c)
LIST(APPEND source src/avp/bh_swdor.c)
LIST(APPEND source src/avp/bh_track.c)
LIST(APPEND source src/avp/bh_types.c)
LIST(APPEND source src/avp/bh_videoscreen.c)
LIST(APPEND source src/avp/bh_waypt.c)
LIST(APPEND source src/avp/bh_weap.c)
LIST(APPEND source src/avp/bh_xeno.c)
LIST(APPEND source src/avp/bonusabilities.c)
LIST(APPEND source src/avp/cconvars.cpp)
LIST(APPEND source src/avp/cdtrackselection.cpp)
LIST(APPEND source src/avp/cheatmodes.c)
LIST(APPEND source src/avp/comp_map.c)
LIST(APPEND source src/avp/comp_shp.c)
LIST(APPEND source src/avp/consolelog.cpp)
LIST(APPEND source src/avp/davehook.cpp)
LIST(APPEND source src/avp/deaths.c)
LIST(APPEND source src/avp/decal.c)
LIST(APPEND source src/avp/detaillevels.c)
LIST(APPEND source src/avp/dynamics.c)
LIST(APPEND source src/avp/dynblock.c)
LIST(APPEND source src/avp/equipmnt.c)
LIST(APPEND source src/avp/extents.c)
LIST(APPEND source src/avp/game.c)
LIST(APPEND source src/avp/game_statistics.c)
LIST(APPEND source src/avp/gamecmds.cpp)
LIST(APPEND source src/avp/gamevars.cpp)
LIST(APPEND source src/avp/hmodel.c)
LIST(APPEND source src/avp/hud.c)
LIST(APPEND source src/avp/inventry.c)
LIST(APPEND source src/avp/language.c)
LIST(APPEND source src/avp/lighting.c)
LIST(APPEND source src/avp/load_shp.c)
LIST(APPEND source src/avp/los.c)
LIST(APPEND source src/avp/mempool.c)
LIST(APPEND source src/avp/messagehistory.c)
LIST(APPEND source src/avp/missions.cpp)
LIST(APPEND source src/avp/movement.c)
LIST(APPEND source src/avp/paintball.c)
LIST(APPEND source src/avp/particle.c)
LIST(APPEND source src/avp/pfarlocs.c)
LIST(APPEND source src/avp/pheromon.c)
LIST(APPEND source src/avp/player.c)
LIST(APPEND source src/avp/pmove.c)
LIST(APPEND source src/avp/psnd.c)
LIST(APPEND source src/avp/psndproj.c)
LIST(APPEND source src/avp/pvisible.c)
LIST(APPEND source src/avp/savegame.c)
LIST(APPEND source src/avp/scream.cpp)
LIST(APPEND source src/avp/secstats.c)
LIST(APPEND source src/avp/sfx.c)
LIST(APPEND source src/avp/stratdef.c)
LIST(APPEND source src/avp/targeting.c)
LIST(APPEND source src/avp/track.c)
LIST(APPEND source src/avp/triggers.c)
LIST(APPEND source src/avp/weapons.c)
LIST(APPEND source src/avp/shapes/cube.c)
LIST(APPEND source src/avp/support/consbind.cpp)
LIST(APPEND source src/avp/support/consbtch.cpp)
LIST(APPEND source src/avp/support/coordstr.cpp)
LIST(APPEND source src/avp/support/daemon.cpp)
LIST(APPEND source src/avp/support/indexfnt.cpp)
LIST(APPEND source src/avp/support/r2base.cpp)
LIST(APPEND source src/avp/support/r2pos666.cpp)
LIST(APPEND source src/avp/support/reflist.cpp)
LIST(APPEND source src/avp/support/refobj.cpp)
LIST(APPEND source src/avp/support/rentrntq.cpp)
LIST(APPEND source src/avp/support/scstring.cpp)
LIST(APPEND source src/avp/support/strtab.cpp)
LIST(APPEND source src/avp/support/strutil.c)
LIST(APPEND source src/avp/support/trig666.cpp)
LIST(APPEND source src/avp/support/wrapstr.cpp)
LIST(APPEND source src/avp/win95/avpchunk.cpp)
LIST(APPEND source src/avp/win95/cheat.c)
LIST(APPEND source src/avp/win95/chtcodes.cpp)
LIST(APPEND source src/avp/win95/d3d_hud.cpp)
LIST(APPEND source src/avp/win95/ddplat.cpp)
LIST(APPEND source src/avp/win95/endianio.c)
LIST(APPEND source src/avp/win95/ffread.cpp)
LIST(APPEND source src/avp/win95/ffstdio.cpp)
LIST(APPEND source src/avp/win95/gammacontrol.cpp)
LIST(APPEND source src/avp/win95/hierplace.cpp)
LIST(APPEND source src/avp/win95/iofocus.cpp)
LIST(APPEND source src/avp/win95/jsndsup.cpp)
LIST(APPEND source src/avp/win95/kzsort.c)
LIST(APPEND source src/avp/win95/langplat.c)
LIST(APPEND source src/avp/win95/modcmds.cpp)
LIST(APPEND source src/avp/win95/npcsetup.cpp)
LIST(APPEND source src/avp/win95/objsetup.cpp)
LIST(APPEND source src/avp/win95/pathchnk.cpp)
LIST(APPEND source src/avp/win95/platsup.c)
LIST(APPEND source src/avp/win95/pldghost.c)
LIST(APPEND source src/avp/win95/pldnet.c)
LIST(APPEND source src/avp/win95/progress_bar.cpp)
LIST(APPEND source src/avp/win95/projload.cpp)
LIST(APPEND source src/avp/win95/scrshot.cpp)
LIST(APPEND source src/avp/win95/strachnk.cpp)
LIST(APPEND source src/avp/win95/system.c)
LIST(APPEND source src/avp/win95/usr_io.c)
LIST(APPEND source src/avp/win95/vision.c)
LIST(APPEND source src/avp/win95/frontend/avp_envinfo.c)
LIST(APPEND source src/avp/win95/frontend/avp_intro.cpp)
LIST(APPEND source src/avp/win95/frontend/avp_menudata.c)
LIST(APPEND source src/avp/win95/frontend/avp_menus.c)
LIST(APPEND source src/avp/win95/frontend/avp_mp_config.cpp)
LIST(APPEND source src/avp/win95/frontend/avp_userprofile.cpp)
LIST(APPEND source src/avp/win95/gadgets/ahudgadg.cpp)
LIST(APPEND source src/avp/win95/gadgets/conscmnd.cpp)
LIST(APPEND source src/avp/win95/gadgets/conssym.cpp)
LIST(APPEND source src/avp/win95/gadgets/consvar.cpp)
LIST(APPEND source src/avp/win95/gadgets/gadget.cpp)
LIST(APPEND source src/avp/win95/gadgets/hudgadg.cpp)
LIST(APPEND source src/avp/win95/gadgets/rootgadg.cpp)
LIST(APPEND source src/avp/win95/gadgets/t_ingadg.cpp)
LIST(APPEND source src/avp/win95/gadgets/teletype.cpp)
LIST(APPEND source src/avp/win95/gadgets/textexp.cpp)
LIST(APPEND source src/avp/win95/gadgets/textin.cpp)
LIST(APPEND source src/avp/win95/gadgets/trepgadg.cpp)
LIST(APPEND source src/win95/animchnk.cpp)
LIST(APPEND source src/win95/animobs.cpp)
LIST(APPEND source src/win95/awtexld.cpp)
LIST(APPEND source src/win95/awbmpld.cpp)
LIST(APPEND source src/win95/awiffld.cpp)
LIST(APPEND source src/win95/awpnmld.cpp)
LIST(APPEND source src/win95/bmpnames.cpp)
LIST(APPEND source src/win95/chnkload.cpp)
LIST(APPEND source src/win95/chnktexi.cpp)
LIST(APPEND source src/win95/chnktype.cpp)
LIST(APPEND source src/win95/chunk.cpp)
LIST(APPEND source src/win95/chunkpal.cpp)
LIST(APPEND source src/win95/db.c)
LIST(APPEND source src/win95/debuglog.cpp)
LIST(APPEND source src/win95/dummyobjectchunk.cpp)
LIST(APPEND source src/win95/enumchnk.cpp)
LIST(APPEND source src/win95/enumsch.cpp)
LIST(APPEND source src/win95/envchunk.cpp)
LIST(APPEND source src/win95/fail.c)
LIST(APPEND source src/win95/fragchnk.cpp)
LIST(APPEND source src/win95/gsprchnk.cpp)
LIST(APPEND source src/win95/hierchnk.cpp)
LIST(APPEND source src/win95/huffman.cpp)
LIST(APPEND source src/win95/iff.cpp)
LIST(APPEND source src/win95/iff_ilbm.cpp)
LIST(APPEND source src/win95/ilbm_ext.cpp)
LIST(APPEND source src/win95/io.c)
LIST(APPEND source src/win95/list_tem.cpp)
LIST(APPEND source src/win95/ltchunk.cpp)
LIST(APPEND source src/win95/media.cpp)
LIST(APPEND source src/win95/mishchnk.cpp)
LIST(APPEND source src/win95/obchunk.cpp)
LIST(APPEND source src/win95/oechunk.cpp)
LIST(APPEND source src/win95/our_mem.c)
LIST(APPEND source src/win95/plat_shp.c)
LIST(APPEND source src/win95/plspecfn.c)
LIST(APPEND source src/win95/shpchunk.cpp)
LIST(APPEND source src/win95/sndchunk.cpp)
LIST(APPEND source src/win95/sprchunk.cpp)
LIST(APPEND source src/win95/string.cpp)
LIST(APPEND source src/win95/texio.c)
LIST(APPEND source src/win95/toolchnk.cpp)
LIST(APPEND source src/win95/txioctrl.cpp)
LIST(APPEND source src/win95/wpchunk.cpp)
LIST(APPEND source src/win95/zsp.cpp)
LIST(APPEND source src/opengl.c)
LIST(APPEND source src/oglfunc.c)
LIST(APPEND source src/fmv.c)
LIST(APPEND source src/openal.c)
LIST(APPEND source src/cdplayer.c)
LIST(APPEND source src/menus.c)
LIST(APPEND source src/main.c)
#LIST(APPEND source src/avp/xr_camera.c)

### build all source as C++
### (not normally used)
##SET_SOURCE_FILES_PROPERTIES(${source} PROPERTIES LANGUAGE CXX)

if(WIN32)
    # Attach the Windows resource script (app icon + version info) to the actual
    # target source list. It was appended to AVP_SOURCES — a variable nothing uses —
    # so ADD_EXECUTABLE(avp ${source}) never compiled it and NEITHER the x64 nor x86
    # exe got the icon/version. Appending to `source` (before the target below) fixes
    # both architectures, since they share this CMakeLists.
    list(APPEND source ../platform/windows/Resource.rc)
endif()

# auto-include directories with source files
FOREACH(sourcefile IN LISTS source)
    GET_FILENAME_COMPONENT(includedir ${sourcefile} PATH) # newer CMake uses DIRECTORY
    LIST(APPEND include ${includedir})
ENDFOREACH(sourcefile)
INCLUDE_DIRECTORIES(${include})

# manually include src/include
INCLUDE_DIRECTORIES(src/include)

if (WIN32)
    ADD_EXECUTABLE(avp ${source})
endif ()

if (AVP_LINUX)
    ADD_EXECUTABLE(avp ${source})
endif ()

if (AVP_MAC)
    ADD_EXECUTABLE(avp ${source})
endif ()

if (ANDROID)
    ADD_LIBRARY(avpvr SHARED ${source})
endif ()

# required dependencies
INCLUDE_DIRECTORIES(${SDL3_INCLUDE})

IF(OPENGL_TYPE STREQUAL "OPENGL")
  INCLUDE_DIRECTORIES(${OPENGL_INCLUDE_DIR})
ENDIF(OPENGL_TYPE STREQUAL "OPENGL")

IF(OPENGL_TYPE STREQUAL "OPENGLES2")
  ADD_DEFINITIONS(-DUSE_OPENGL_ES=3)
  INCLUDE_DIRECTORIES(${OPENGLES_INCLUDE_DIR})
  INCLUDE_DIRECTORIES(${EGL_INCLUDE_DIR})
ENDIF(OPENGL_TYPE STREQUAL "OPENGLES2")

INCLUDE_DIRECTORIES(${OPENAL_INCLUDE_DIR})

if (ANDROID)
    TARGET_LINK_LIBRARIES(avpvr ${SDL3_LIBRARY})
endif ()
if (WIN32)
    TARGET_LINK_LIBRARIES(avp ${SDL3_LIBRARY})
endif ()

if (ANDROID)
    IF(OPENGL_TYPE STREQUAL "OPENGL")
      TARGET_LINK_LIBRARIES(avpvr ${OPENGL_gl_LIBRARY})
    ENDIF(OPENGL_TYPE STREQUAL "OPENGL")
endif ()

if (WIN32)
    IF(OPENGL_TYPE STREQUAL "OPENGL")
      TARGET_LINK_LIBRARIES(avp ${OPENGL_gl_LIBRARY})
    ENDIF(OPENGL_TYPE STREQUAL "OPENGL")
endif ()

IF(OPENGL_TYPE STREQUAL "OPENGLES2")
  TARGET_LINK_LIBRARIES(avpvr ${OPENGLES_gl_LIBRARY})
  TARGET_LINK_LIBRARIES(avpvr ${EGL_LIBRARIES})
ENDIF(OPENGL_TYPE STREQUAL "OPENGLES2")

if (ANDROID)
    TARGET_LINK_LIBRARIES(avpvr ${OPENAL_LIBRARY})
endif ()

if (WIN32)
    TARGET_LINK_LIBRARIES(avp ${OPENAL_LIBRARY})
endif ()

#WINDOWS LIBRARIES
if (WIN32)
    target_link_libraries(avp shlwapi.lib winmm.lib ws2_32.lib)

    # FFmpeg: fmv.c's desktop FMV path (PlayBinkedFMV / PlayMenuMusic /
    # InitialiseTriggeredFMVs) links against these import libs. Wired in here so a
    # CMake regenerate no longer wipes the manual VS "Additional Library
    # Directories" (the matching -62/-60/etc. runtime DLLs must sit next to
    # avp.exe; headers come from extern/FFmpeg/include, already on the include path).
    target_link_libraries(avp
        "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/avcodec.lib"
        "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/avformat.lib"
        "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/avutil.lib"
        "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/swscale.lib"
        "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${ARCH_DIR}/swresample.lib"
    )
endif ()

#LINUX / MACOS LINK LIBRARIES
if (AVP_LINUX OR AVP_MAC)
    # SDL3 and OpenAL are the bundled extern/*/lib/<os>/<arch> shared libs when present,
    # or the system ones (FindSDL3.cmake / FindOpenAL) as a fallback. FFmpeg is either the
    # bundled set or the pkg-config imported target — never a mix of the two.
    if (AVP_FFMPEG_IS_BUNDLED)
        set(AVP_FFMPEG_LINK ${AVP_FFMPEG_BUNDLED_LIBS})
    else()
        set(AVP_FFMPEG_LINK PkgConfig::FFMPEG)
    endif()

    # Use the imported target, not ${OPENGL_gl_LIBRARY}: on a GLVND system (any current
    # Ubuntu/Debian/Fedora) FindOpenGL sets OPENGL_opengl_LIBRARY + OPENGL_glx_LIBRARY
    # and leaves OPENGL_gl_LIBRARY EMPTY, so the raw-variable form links no GL at all
    # and every glBindTexture/glViewport comes back undefined at link time.
    # OpenGL::GL resolves to libOpenGL+libGLX there and to plain libGL on legacy setups.
    if (TARGET OpenGL::GL)
        set(AVP_GL_LINK OpenGL::GL)
    else()
        set(AVP_GL_LINK ${OPENGL_gl_LIBRARY})
    endif()

    target_link_libraries(avp
        ${SDL3_LIBRARY}
        ${AVP_GL_LINK}
        ${OPENAL_LIBRARY}
        ${AVP_FFMPEG_LINK}
        ${CMAKE_DL_LIBS}
        m
        pthread
    )
endif ()

#ANDROID LINK LIBRARIES
if (ANDROID)
    add_definitions(
        -DXR_USE_PLATFORM_ANDROID
        -DXR_USE_GRAPHICS_API_OPENGL_ES
        -DGL_GLEXT_PROTOTYPES
    )
    TARGET_LINK_LIBRARIES(avpvr
            ${log-lib}
            GLESv3
            EGL
            openxr_loader
            avcodec
            avformat
            avutil
            swscale
    )
endif ()

#COPY FILES TO RELEVANT BUILD FOLDERS
# We only define an install target if we're doing a release build.
if (AVP_MAC)
    install (TARGETS avp DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})

    # Mirror the Windows/Linux layout: one flat folder per arch under build/mac,
    # holding the binary, its bundled dylibs and the assets. Named per-arch
    # (avp_x64 / avp_arm64) so the builds can coexist.
    #
    # This deliberately replaces the old .app-bundle path, which never worked: it
    # referenced an undefined MACOS_ASSETS_ICON, copied a source/data folder that does
    # not exist in this repo, addressed target "avp" while set_target_properties named
    # "avpvr", left MACOSX_BUNDLE commented out, and required dylibbundler. A relocatable
    # folder matches how build-linux.sh already ships Linux. Wrap it in a .app later if
    # you want a double-clickable icon (source/src/avp.icns is still here for that).
    set_target_properties(avp PROPERTIES
        OUTPUT_NAME "avp_${ARCH_DIR}"
        # Anchored to the source tree, not the build dir, for the same reason as Linux:
        # a macOS build tree can live anywhere and relative hops out of it misresolve.
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../build/mac/${ARCH_DIR}"
        # The macOS answer to Linux's $ORIGIN. The bundled dylibs record their deps as
        # "@rpath/libavutil.60.dylib", and dyld resolves @rpath against the run-path list
        # of the loading image AND of the main executable — so one LC_RPATH on the exe
        # covers the whole chain, including libavcodec's own reference to libavutil.
        # BUILD_WITH_INSTALL_RPATH applies it to the build-tree binary too; the default
        # otherwise bakes in absolute paths back into source/extern and the folder stops
        # being relocatable the moment it is copied or zipped.
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path")

    # Assets are not committed, so only wire the copy up when they are actually there
    # — an unconditional copy_directory would fail the build on a fresh clone.
    if (EXISTS "${CMAKE_SOURCE_DIR}/../assets")
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/../assets"
                    "$<TARGET_FILE_DIR:avp>"
            COMMENT "Updating assets next to avp_${ARCH_DIR}"
            VERBATIM)
    else()
        message(STATUS "AVP: no assets/ folder found — skipping the asset copy step.")
    endif()

    # AVP_OTOOL was located during FFmpeg detection, above.

    # The loader asks for a dylib by the leaf of its LC_ID_DYLIB install name, not by the
    # filename we linked against: extern/.../libavcodec.dylib has an id of
    # "@rpath/libavcodec.62.dylib" and libopenal.dylib has "@rpath/libopenal.1.dylib".
    # Copying only the bare libfoo.dylib therefore builds fine and then dies at startup
    # with "Library not loaded". Stage each lib under BOTH names — the Mach-O counterpart
    # of the SONAME staging the Linux branch does with readelf.
    function(avp_stage_mac_lib lib)
        get_filename_component(_name "${lib}" NAME)
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${lib}" "$<TARGET_FILE_DIR:avp>/${_name}"
            VERBATIM)
        # Accumulate every name we stage so the prune step below knows what is current.
        list(APPEND AVP_STAGED_NAMES "${_name}")
        set(AVP_STAGED_NAMES "${AVP_STAGED_NAMES}" PARENT_SCOPE)

        set(_id_name "")
        if (AVP_OTOOL)
            # otool -D prints "<path>:\n<install name>\n"; take the second line's leaf.
            execute_process(COMMAND "${AVP_OTOOL}" -D "${lib}"
                            OUTPUT_VARIABLE _out ERROR_QUIET RESULT_VARIABLE _rc)
            if (_rc EQUAL 0 AND _out MATCHES "\n([^\n]+)")
                get_filename_component(_id_name "${CMAKE_MATCH_1}" NAME)
            endif()
        endif()

        if (_id_name AND NOT _id_name STREQUAL _name)
            add_custom_command(TARGET avp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${lib}" "$<TARGET_FILE_DIR:avp>/${_id_name}"
                VERBATIM)
            list(APPEND AVP_STAGED_NAMES "${_id_name}")
            set(AVP_STAGED_NAMES "${AVP_STAGED_NAMES}" PARENT_SCOPE)
        elseif (NOT AVP_OTOOL)
            message(WARNING
                "otool not found — cannot read the install name of ${_name}. If the game "
                "starts with \"Library not loaded: @rpath/...\", copy that lib next to the "
                "binary under the name in the error (e.g. libavcodec.62.dylib) by hand.")
        endif()
    endfunction()

    if (SDL3_LIBRARY MATCHES "extern/SDL3/lib/mac/")
        avp_stage_mac_lib("${AVP_SDL3_BUNDLED}")
    endif()
    if (AVP_OPENAL_IS_BUNDLED)
        avp_stage_mac_lib("${AVP_OPENAL_BUNDLED}")
    endif()
    if (AVP_FFMPEG_IS_BUNDLED)
        foreach (avp_lib ${AVP_FFMPEG_BUNDLED_LIBS})
            avp_stage_mac_lib("${avp_lib}")
        endforeach()
    endif()

    # Same stale-file problem as the other platforms: an FFmpeg major bump leaves
    # libavcodec.62.dylib sitting next to the new libavcodec.63.dylib, and as on Linux a
    # stale versioned name CAN be picked up — it is exactly what some other lib may ask
    # the loader for. Registered last so it prunes against what staging just wrote.
    if (AVP_STAGED_NAMES)
        string(REPLACE ";" "|" AVP_STAGED_NAMES_ARG "${AVP_STAGED_NAMES}")
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    "-DPRUNE_DIR=$<TARGET_FILE_DIR:avp>"
                    "-DPRUNE_GLOBS=libavcodec*.dylib|libavformat*.dylib|libavutil*.dylib|libswscale*.dylib|libswresample*.dylib|libavfilter*.dylib|libavdevice*.dylib|libSDL3*.dylib|libopenal*.dylib"
                    "-DPRUNE_KEEP=${AVP_STAGED_NAMES_ARG}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/prune-stale-libs.cmake"
            COMMENT "Pruning stale bundled dylibs next to avp_${ARCH_DIR}"
            VERBATIM)
    endif()
elseif (WIN32)
    # Reuse the arch resolved at the top of the file (x64 / x86 / arm64) so the exe
    # name, output dir and DLL copy all agree with the lib folders we linked from.
    set(AVP_ARCH ${ARCH_DIR})

    if(AVP_ARCH STREQUAL "x86")
        # The bundled FFmpeg import libs (extern/FFmpeg/lib/windows/x86) are
        # MinGW-built and carry no SAFESEH data, so a 32-bit MSVC link fails with
        # LNK2026 "module unsafe for SAFESEH image" -> LNK1281 "Unable to generate
        # SAFESEH image". Disable SAFESEH for the x86 build only (x64 and arm64 have
        # no SAFESEH concept, so this never applies there). Set here so it survives a
        # CMake regenerate.
        if(MSVC)
            target_link_options(avp PRIVATE /SAFESEH:NO)
        endif()
    endif()
    
    install (TARGETS avp DESTINATION .)

    # The PCVR build gets its own exe name AND its own output folder
    # (build/windows/<arch>vr), fully self-contained: the post-build steps below
    # all stage into $<TARGET_FILE_DIR:avp>, so the DLLs, loader and assets
    # follow the folder automatically. Kept separate from the flat build so the
    # two never fight over config.cfg / user_profiles (the game writes next to
    # its assets — see "Where the game writes" in CLAUDE.md).
    if (AVP_ENABLE_PCVR)
        set(AVP_EXE_NAME "avp_${AVP_ARCH}vr")
        set(AVP_OUT_DIR  "${AVP_ARCH}vr")
    else()
        set(AVP_EXE_NAME "avp_${AVP_ARCH}")
        set(AVP_OUT_DIR  "${AVP_ARCH}")
    endif()

    # Place the exe and all accompanying files inside a avp/ subfolder,
    # mirroring the avp.app bundle layout on macOS.
    set_target_properties(avp PROPERTIES
        # Name the exe per-arch (avp_x64.exe / avp_x86.exe / avp_arm64.exe —
        # avp_<arch>vr.exe for the PCVR build) so the
        # builds can coexist. Set here (not in the generated .vcxproj) so it
        # survives a CMake regenerate.
        OUTPUT_NAME "${AVP_EXE_NAME}"
        # Anchored to the source tree rather than the build dir, so it lands in
        # <repo>/build/windows/<arch> (build/windows/<arch>vr for PCVR) no matter
        # where CMake was pointed. Resolves to
        # the same place as before for the documented platform/windows/<arch> layout.
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../build/windows/${AVP_OUT_DIR}"
        # Launch the VS debugger (F5 / "Local Windows Debugger") with the CWD set
        # to the exe's own folder, so it finds the assets copied next to it —
        # matching how it behaves when run from Explorer. Without this the
        # debugger uses the project dir and the game can't locate its data.
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/../build/windows/${AVP_OUT_DIR}"
    )

    # Runtime DLLs live in the target sysroot bin, not the host compiler bin.
    # MINGW_PREFIX and MINGW_TRIPLE are defined in cmake/toolchain-mingw64.cmake.
    set(MINGW_RUNTIME_DIR "${MINGW_PREFIX}/${MINGW_TRIPLE}/bin")    
    
    # 1. Copy SDL3 DLL
    add_custom_command(TARGET avp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/windows/${AVP_ARCH}/SDL3.dll"
                "$<TARGET_FILE_DIR:avp>"
        COMMENT "Copying SDL3 runtime DLL next to avp.exe"
        VERBATIM)

    # 1a. PCVR: stage the OpenXR loader next to the exe. main.c LoadLibraryA's
    # it at startup; without it the exe silently runs flat.
    if (AVP_ENABLE_PCVR)
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AVP_OPENXR_DLL}"
                    "$<TARGET_FILE_DIR:avp>"
            COMMENT "Copying openxr_loader.dll next to ${AVP_EXE_NAME}.exe"
            VERBATIM)
    endif()

    # 1b. Copy the FFmpeg runtime DLLs. Glob them per-arch rather than naming them:
    # the versioned suffixes differ between the bundled builds (x64/x86 ship
    # avcodec-62 / avutil-60 / swscale-9 / swresample-6 / avformat-62, arm64 ships
    # avcodec-63 / avutil-61 / swscale-10 / swresample-7 / avformat-63), so a
    # hardcoded list would silently copy nothing on one of them.
    file(GLOB AVP_FFMPEG_DLLS "${CMAKE_SOURCE_DIR}/extern/FFmpeg/lib/windows/${AVP_ARCH}/*.dll")
    if (AVP_FFMPEG_DLLS)
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${AVP_FFMPEG_DLLS}
                    "$<TARGET_FILE_DIR:avp>"
            COMMENT "Copying FFmpeg runtime DLLs next to avp.exe"
            VERBATIM)
    else()
        message(WARNING "No FFmpeg runtime DLLs found in extern/FFmpeg/lib/windows/"
                        "${AVP_ARCH} — avp_${AVP_ARCH}.exe will fail to start until the "
                        "matching av*/sw* DLLs are placed next to it.")
    endif()

    # 1c. Drop DLLs a previous build left behind. copy_if_different never removes, so
    # after an FFmpeg major bump the folder holds both avcodec-62.dll and avcodec-63.dll;
    # the exe loads the right one, but the wrong one still ships. Registered after the
    # copies so it prunes against what was just staged.
    set(AVP_KEEP_DLLS "SDL3.dll")
    foreach (avp_dll ${AVP_FFMPEG_DLLS})
        get_filename_component(avp_dll_name "${avp_dll}" NAME)
        list(APPEND AVP_KEEP_DLLS "${avp_dll_name}")
    endforeach()
    string(REPLACE ";" "|" AVP_KEEP_DLLS_ARG "${AVP_KEEP_DLLS}")

    add_custom_command(TARGET avp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DPRUNE_DIR=$<TARGET_FILE_DIR:avp>"
                "-DPRUNE_GLOBS=avcodec-*.dll|avformat-*.dll|avutil-*.dll|swscale-*.dll|swresample-*.dll|avfilter-*.dll|avdevice-*.dll"
                "-DPRUNE_KEEP=${AVP_KEEP_DLLS_ARG}"
                -P "${CMAKE_SOURCE_DIR}/cmake/prune-stale-libs.cmake"
        COMMENT "Pruning stale FFmpeg runtime DLLs next to avp.exe"
        VERBATIM)

    # 2. Copy Assets (Removed the 'rm -rf' line)
    add_custom_command(TARGET avp POST_BUILD
        # Ensure the directory exists (safe to run even if it does)
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:avp>"
        # Copy the directory content
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}../../assets"
                "$<TARGET_FILE_DIR:avp>"
        COMMENT "Updating assets next to avp.exe"
        VERBATIM)
elseif (AVP_LINUX)
    install (TARGETS avp DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})

    # Mirror the Windows layout: one folder per arch under build/linux, holding the
    # binary, its bundled .so's and the assets. Named per-arch (avp_x64 / avp_x86 /
    # avp_arm64) so the three builds can coexist.
    set_target_properties(avp PROPERTIES
        OUTPUT_NAME "avp_${ARCH_DIR}"
        # Anchored to the source tree, NOT to the build dir: a Linux build tree can
        # live anywhere (/tmp/avp-x64, ~/build, ...), and the old
        # CMAKE_CURRENT_BINARY_DIR/../../../ form silently resolved to /build and
        # failed the link with "mkdir(/build): Permission denied".
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../build/linux/${ARCH_DIR}"
        # Any bundled extern/*/lib/linux/<arch>/*.so we linked is copied next to the
        # binary and found via an $ORIGIN rpath, so the game runs without those libs
        # being installed system-wide. BUILD_WITH_INSTALL_RPATH TRUE applies $ORIGIN to
        # the build-tree binary too — the default bakes in absolute paths back into
        # source/extern, which works on this machine but makes build/linux/<arch>
        # non-portable the moment it is copied or zipped up.
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN")

    # Emit DT_RPATH rather than the modern DT_RUNPATH. DT_RUNPATH applies ONLY to the
    # direct dependencies of the object that carries it, so the exe's $ORIGIN does not
    # cover libraries pulled in *by* a staged lib: libavcodec needs libswresample, and
    # even with libswresample.so.6 sitting right there it fails with "not found".
    # DT_RPATH is inherited down the whole dependency chain, which is what we want for
    # a self-contained folder.
    target_link_options(avp PRIVATE "-Wl,--disable-new-dtags")

    # Assets are not committed, so only wire the copy up when they are actually there
    # — an unconditional copy_directory would fail the build on a fresh clone.
    if (EXISTS "${CMAKE_SOURCE_DIR}/../assets")
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/../assets"
                    "$<TARGET_FILE_DIR:avp>"
            COMMENT "Updating assets next to avp_${ARCH_DIR}"
            VERBATIM)
    else()
        message(STATUS "AVP: no assets/ folder found — skipping the asset copy step.")
    endif()

    # AVP_READELF was located during FFmpeg detection, above.

    # The dynamic loader asks for a library by its SONAME, not by the filename we
    # linked against: linking extern/.../libSDL3.so records a DT_NEEDED of
    # "libSDL3.so.0", and FFmpeg's plain libavcodec.so records "libavcodec.so.63".
    # Copying only the bare libfoo.so therefore builds fine and then dies at startup
    # with "cannot open shared object file". Stage each lib under BOTH names.
    function(avp_stage_linux_lib lib)
        get_filename_component(_name "${lib}" NAME)
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${lib}" "$<TARGET_FILE_DIR:avp>/${_name}"
            VERBATIM)
        # Accumulate every name we stage so the prune step below knows what is current.
        list(APPEND AVP_STAGED_NAMES "${_name}")
        set(AVP_STAGED_NAMES "${AVP_STAGED_NAMES}" PARENT_SCOPE)

        set(_soname "")
        if (AVP_READELF)
            execute_process(COMMAND "${AVP_READELF}" -d "${lib}"
                            OUTPUT_VARIABLE _dyn ERROR_QUIET RESULT_VARIABLE _rc)
            if (_rc EQUAL 0 AND _dyn MATCHES "SONAME[^\n]*\\[([^]]+)\\]")
                set(_soname "${CMAKE_MATCH_1}")
            endif()
        endif()

        if (_soname AND NOT _soname STREQUAL _name)
            add_custom_command(TARGET avp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${lib}" "$<TARGET_FILE_DIR:avp>/${_soname}"
                VERBATIM)
            list(APPEND AVP_STAGED_NAMES "${_soname}")
            set(AVP_STAGED_NAMES "${AVP_STAGED_NAMES}" PARENT_SCOPE)
        elseif (NOT AVP_READELF)
            message(WARNING
                "readelf not found — cannot resolve the SONAME of ${_name}. If the game "
                "starts with \"cannot open shared object file\", copy that lib next to "
                "the exe under its SONAME (e.g. libSDL3.so.0) by hand.")
        endif()
    endfunction()

    if (SDL3_LIBRARY MATCHES "extern/SDL3/lib/linux/")
        avp_stage_linux_lib("${AVP_SDL3_BUNDLED}")
    endif()
    if (AVP_OPENAL_IS_BUNDLED)
        avp_stage_linux_lib("${AVP_OPENAL_BUNDLED}")
    endif()
    if (AVP_FFMPEG_IS_BUNDLED)
        foreach (avp_lib ${AVP_FFMPEG_BUNDLED_LIBS})
            avp_stage_linux_lib("${avp_lib}")
        endforeach()
    endif()

    # Same stale-file problem as Windows, made worse by SONAMEs: an FFmpeg major bump
    # leaves libavcodec.so.63 sitting next to the new libavcodec.so.62, and unlike a
    # Windows DLL a stale soname CAN be picked up — it is exactly the name some other
    # library might ask the loader for. Registered last so it prunes against what the
    # staging steps above just wrote.
    if (AVP_STAGED_NAMES)
        string(REPLACE ";" "|" AVP_STAGED_NAMES_ARG "${AVP_STAGED_NAMES}")
        add_custom_command(TARGET avp POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    "-DPRUNE_DIR=$<TARGET_FILE_DIR:avp>"
                    "-DPRUNE_GLOBS=libavcodec.so*|libavformat.so*|libavutil.so*|libswscale.so*|libswresample.so*|libavfilter.so*|libavdevice.so*|libSDL3.so*|libopenal.so*"
                    "-DPRUNE_KEEP=${AVP_STAGED_NAMES_ARG}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/prune-stale-libs.cmake"
            COMMENT "Pruning stale bundled .so's next to avp_${ARCH_DIR}"
            VERBATIM)
    endif()
else ()
    # We use the default installation directory
    install (TARGETS avpvr DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})
endif ()