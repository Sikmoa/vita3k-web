# cmake -DWEB_DIR=<dir> -DSOURCES=<files> -DSTAMP=<file> -P check_web_imports.cmake
#
# Fails when a script the browser loads by a literal relative URL is not in
# the deployable set (every file in WEB_DIR): static and dynamic ES imports and
# Worker scripts in the web files, and EM_JS imports in the module SOURCES
# (gxm_webgpu_bridge.cpp imports gxm_scene.js next to the Worker).
cmake_minimum_required(VERSION 3.22) # policies for `-P`: IN_LIST needs CMP0057
file(GLOB _deployed LIST_DIRECTORIES false RELATIVE "${WEB_DIR}" "${WEB_DIR}/*")
file(GLOB _web_files "${WEB_DIR}/*.js" "${WEB_DIR}/*.html")
set(_pattern "(from|import|new Worker) *\\(? *(new URL\\( *)?['\"](\\./)?([A-Za-z0-9_.-]+)['\"]")
set(_missing "")
foreach(_file IN LISTS _web_files SOURCES)
    file(STRINGS "${_file}" _lines REGEX "${_pattern}")
    string(REGEX MATCHALL "${_pattern}" _references "${_lines}")
    foreach(_reference IN LISTS _references)
        string(REGEX REPLACE "${_pattern}" "\\4" _name "${_reference}")
        if(NOT _name IN_LIST _deployed)
            list(APPEND _missing "${_name} (loaded by ${_file})")
        endif()
    endforeach()
endforeach()
if(_missing)
    list(JOIN _missing "\n  " _missing)
    message(FATAL_ERROR "Not in the deployable set ${WEB_DIR}:\n  ${_missing}")
endif()
file(TOUCH "${STAMP}")
