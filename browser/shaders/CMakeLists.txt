# Standalone GXP -> SPIR-V compiler (the production USSE translator, no GUI,
# renderer, guest CPU or HLE). With the Emscripten toolchain it is the browser
# module gxp_compiler.mjs/.wasm that gxp_shader_adapter.js loads; natively it
# is the gxp_compile oracle (gxp_compile <in.gxp> <out.spv>).
#
# The browser build (browser/CMakeLists.txt) builds it as a separate wasm32
# project even in a Memory64 build: the adapter drives it with Number
# pointers, so it must stay wasm32.
cmake_minimum_required(VERSION 3.22)
project(vita3k_gxp_compiler CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

file(GLOB _shader_sources CONFIGURE_DEPENDS
    "${_repo}/vita3k/shader/src/*.cpp" "${_repo}/vita3k/shader/src/translator/*.cpp")
add_executable(gxp_compiler
    gxp_compile.cpp
    ${_shader_sources}
    "${_repo}/vita3k/gxm/src/gxp.cpp"
    "${_repo}/vita3k/gxm/src/attributes.cpp"
    "${_repo}/vita3k/gxm/src/color.cpp"
    "${_repo}/vita3k/gxm/src/textures.cpp"
    "${_repo}/external/glslang/SPIRV/SpvBuilder.cpp"
    "${_repo}/external/glslang/SPIRV/SpvPostProcess.cpp"
    "${_repo}/external/glslang/SPIRV/InReadableOrder.cpp"
    "${_repo}/external/glslang/SPIRV/Logger.cpp"
    "${_repo}/external/glslang/SPIRV/disassemble.cpp"
    "${_repo}/external/glslang/SPIRV/doc.cpp"
    "${_repo}/external/fmt/src/format.cc"
)
# GLSL/SPIRV-Cross output paths are compiled out.
target_compile_definitions(gxp_compiler PRIVATE
    VITA3K_SHADER_SPIRV_ONLY SPDLOG_FMT_EXTERNAL SPDLOG_NO_THREAD_ID SPDLOG_NO_TLS)
target_include_directories(gxp_compiler PRIVATE
    "${_repo}/vita3k/shader/include" "${_repo}/vita3k/gxm/include"
    "${_repo}/vita3k/features/include" "${_repo}/vita3k/util/include"
    "${_repo}/vita3k/mem/include" "${_repo}/external/fmt/include"
    "${_repo}/external/spdlog/include" "${_repo}/external/boost" "${_repo}/external/glslang")
target_compile_options(gxp_compiler PRIVATE -O0 -g0 -ffunction-sections -fdata-sections)

if(EMSCRIPTEN)
    target_compile_options(gxp_compiler PRIVATE -fexceptions)
    target_link_options(gxp_compiler PRIVATE
        -fexceptions -sDISABLE_EXCEPTION_CATCHING=0
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node,worker
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=268435456
        -sFILESYSTEM=0 --no-entry
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_gxp_compile','_gxp_output_data','_gxp_output_size','_gxp_error']"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','HEAPU8','HEAPU32']")
    set_target_properties(gxp_compiler PROPERTIES SUFFIX ".mjs")
else()
    target_link_options(gxp_compiler PRIVATE -Wl,--gc-sections -pthread)
    set_target_properties(gxp_compiler PROPERTIES OUTPUT_NAME gxp_compile)
endif()
