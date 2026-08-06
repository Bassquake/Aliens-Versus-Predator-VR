# Delete runtime libraries left behind in an output folder by a previous build.
#
# The post-build steps use `cmake -E copy_if_different`, which adds files but never
# removes them, so bumping a dependency's major version leaves BOTH sets side by side
# (e.g. avcodec-62.dll next to a stale avcodec-63.dll). The exe loads the right one by
# name, but the folder is what gets shipped, so the stale copies have to go.
#
# Run in script mode from a POST_BUILD step:
#   cmake -DPRUNE_DIR=<dir> -DPRUNE_GLOBS=<a|b> -DPRUNE_KEEP=<x|y> -P prune-stale-libs.cmake
#
# PRUNE_GLOBS  filename patterns this build owns (only these are ever considered)
# PRUNE_KEEP   basenames to keep; anything matching a glob but absent here is removed
#
# Both lists are '|'-separated: a -D value cannot carry ';' without the shell or CMake
# splitting it into separate arguments.
cmake_minimum_required(VERSION 3.22)

if (NOT PRUNE_DIR OR NOT PRUNE_GLOBS)
    message(FATAL_ERROR "prune-stale-libs.cmake: PRUNE_DIR and PRUNE_GLOBS are required")
endif()

string(REPLACE "|" ";" _globs "${PRUNE_GLOBS}")
string(REPLACE "|" ";" _keep  "${PRUNE_KEEP}")

set(_candidates "")
foreach (_g ${_globs})
    file(GLOB _matched "${PRUNE_DIR}/${_g}")
    list(APPEND _candidates ${_matched})
endforeach()

foreach (_f ${_candidates})
    get_filename_component(_name "${_f}" NAME)
    if (NOT _name IN_LIST _keep)
        message(STATUS "Pruning stale runtime lib: ${_name}")
        file(REMOVE "${_f}")
    endif()
endforeach()
