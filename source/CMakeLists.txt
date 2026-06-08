# Aliens vs Predator - https://github.com/atsb/NakedAVP
CMAKE_MINIMUM_REQUIRED(VERSION 3.22.1)

# default to Release
IF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    MESSAGE(STATUS "No build type specified, defaulting to Release")
    SET(CMAKE_BUILD_TYPE Release CACHE STRING "Build type; one of: Debug Release RelWithDebInfo MinSizeRel" FORCE)
    SET_PROPERTY(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)

PROJECT(avpvr)

# required dependencies
if (ANDROID)
    INCLUDE(FindOpenGLES2.cmake)
endif()
INCLUDE(FindSDL3.cmake)
INCLUDE(FindOpenGL)

if (WIN32)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ARCH_DIR "x64")
    else()
        set(ARCH_DIR "x86")
endif()
    set(OPENAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/OpenAL32/include" CACHE FILEPATH "" FORCE)
    set(OPENAL_LIBRARY "${CMAKE_SOURCE_DIR}/extern/OpenAL32/lib/${ARCH_DIR}/OpenAL32.lib" CACHE FILEPATH "" FORCE)
endif()

if (NOT ANDROID)
    include(FindOpenAL)    
else()
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
else()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_DIR "x64")
else()
    set(ARCH_DIR "x86")
endif()
    set(SDL3_LIBRARY "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/${ARCH_DIR}/SDL3.lib" CACHE FILEPATH "" FORCE)
    set(SDL3_INCLUDE "${CMAKE_SOURCE_DIR}/extern" CACHE PATH "" FORCE)
    set(SDL3_FOUND TRUE)
endif()

IF(NOT SDL3_FOUND)
    MESSAGE(FATAL_ERROR "SDL 3.0 was requested but CMake couldn't find it.")
ENDIF(NOT SDL3_FOUND)
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

include_directories(
    ${CMAKE_SOURCE_DIR}/extern/FFmpeg/include
)

#add_library(ffmpeg_jni SHARED src/ffmpeg_jni.c)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libavcodec.so"
)

add_library(avdevice SHARED IMPORTED)
set_target_properties(avdevice PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libavdevice.so"
)

add_library(avfilter SHARED IMPORTED)
set_target_properties(avfilter PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libavfilter.so"
)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libavformat.so"
)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libavutil.so"
)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libswresample.so"
)

add_library(swscale SHARED IMPORTED)
set_target_properties(swscale PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../platform/android/app/src/main/jniLibs/arm64-v8a/libswscale.so"
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

# auto-include directories with source files
FOREACH(sourcefile IN LISTS source)
    GET_FILENAME_COMPONENT(includedir ${sourcefile} PATH) # newer CMake uses DIRECTORY
    LIST(APPEND include ${includedir})
ENDFOREACH(sourcefile)
INCLUDE_DIRECTORIES(${include})

# manually include src/include
INCLUDE_DIRECTORIES(src/include)

if (WIN32)
    ADD_EXECUTABLE(avpvr ${source})
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

TARGET_LINK_LIBRARIES(avpvr ${SDL3_LIBRARY})

IF(OPENGL_TYPE STREQUAL "OPENGL")
  TARGET_LINK_LIBRARIES(avpvr ${OPENGL_gl_LIBRARY})
ENDIF(OPENGL_TYPE STREQUAL "OPENGL")

IF(OPENGL_TYPE STREQUAL "OPENGLES2")
  TARGET_LINK_LIBRARIES(avpvr ${OPENGLES_gl_LIBRARY})
  TARGET_LINK_LIBRARIES(avpvr ${EGL_LIBRARIES})
ENDIF(OPENGL_TYPE STREQUAL "OPENGLES2")

TARGET_LINK_LIBRARIES(avpvr ${OPENAL_LIBRARY})

#WINDOWS LIBRARIES
if (WIN32)
    target_link_libraries(avpvr shlwapi.lib winmm.lib)
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
if (APPLE)
    install (TARGETS avp BUNDLE DESTINATION .)

	set(MACOS_ASSETS "${MACOS_ASSETS_ICON}")
	target_sources(avp PUBLIC ${MACOS_ASSETS})

	# I use this custom command to copy the data folder under the Resources directy
	# I can't make it work using MACOSX_PACKAGE_LOCATION source property
	add_custom_command(TARGET avp POST_BUILD
  						COMMAND ${CMAKE_COMMAND} -E copy_directory
						"${CMAKE_SOURCE_DIR}/data"
						"$<TARGET_FILE_DIR:avp>/../Resources") 

	# Remove files from what has been copied
	add_custom_command(TARGET avp POST_BUILD
						COMMAND ${CMAKE_COMMAND} -E rm
					  "$<TARGET_FILE_DIR:avp>/../Resources/CMakeLists.txt"
					  "$<TARGET_FILE_DIR:avp>/../Resources/avp.ini")

	find_program(DYLIBBUNDLER_EXE dylibbundler REQUIRED)

	add_custom_command(TARGET avp POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
				"$<TARGET_BUNDLE_DIR:avp>/Contents/Frameworks"
		COMMAND ${DYLIBBUNDLER_EXE}
				-od -b -of
				-x "$<TARGET_BUNDLE_DIR:avp>/Contents/MacOS/avp"
				-d "$<TARGET_BUNDLE_DIR:avp>/Contents/Frameworks/"
				-p "@loader_path/../Frameworks/"
		COMMENT "Bundling dylibs into avp.app/Contents/Frameworks"
		VERBATIM)
		
	# Override certain properties to make the avp
	# executable into an application bundle for OS X.
	set_target_properties (avp PROPERTIES
		OUTPUT_NAME "avp"
	#	MACOSX_BUNDLE true
		MACOSX_BUNDLE_BUNDLE_NAME "avp"
		MACOSX_BUNDLE_ICON_FILE "sword.icns"
		MACOSX_BUNDLE_GUI_IDENTIFIER "com.avp.avp"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
		MACOSX_BUNDLE_COPYRIGHT "© 2024 avp Team"
		RESOURCE "${MACOS_ASSETS}"
	)
	
elseif (WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(STATUS "Target is 64-bit")
        set(AVP_ARCH x64)
    else()
        message(STATUS "Target is 32-bit")
        set(AVP_ARCH x86)
    endif()
    
    install (TARGETS avp DESTINATION .)

    # Place the exe and all accompanying files inside a avp/ subfolder,
    # mirroring the avp.app bundle layout on macOS.
    set_target_properties(avp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../../../build/windows/${AVP_ARCH}"
    )

    # Runtime DLLs live in the target sysroot bin, not the host compiler bin.
    # MINGW_PREFIX and MINGW_TRIPLE are defined in cmake/toolchain-mingw64.cmake.
    set(MINGW_RUNTIME_DIR "${MINGW_PREFIX}/${MINGW_TRIPLE}/bin")    
    
    # 1. Copy SDL3 DLL
    add_custom_command(TARGET avp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/extern/SDL3/lib/${AVP_ARCH}/SDL3.dll"
                "$<TARGET_FILE_DIR:avp>"
        COMMENT "Copying SDL3 runtime DLL next to avp.exe"
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
else ()
    # We use the default installation directory
    install (TARGETS avpvr DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})
endif ()