# Genuine VitaSDK GXM surface fixture (browser/tests/gxm_surface_chromium.mjs),
# built like vita_msg_dialog_fixture: the SDK's default crt0, libc and linker
# script, and vita-elf-create + vita-make-fself, output in the binary
# directory. It embeds the repository's own GXP programs
# (tools/native-tool/src/shaders), never game assets.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vita_gxm_surface_fixture LANGUAGES NONE)
endif()

set(VITASDK "$ENV{VITASDK}" CACHE PATH "VitaSDK root (containing bin/arm-vita-eabi-gcc)")
foreach(tool arm-vita-eabi-gcc vita-elf-create vita-make-fself)
    if(NOT EXISTS "${VITASDK}/bin/${tool}")
        message(FATAL_ERROR "Set VITASDK to a VitaSDK installation; missing ${VITASDK}/bin/${tool}")
    endif()
endforeach()

set(_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/native-tool/src/shaders")
set(_shaders color_v color_f texture_v texture_f)
set(_shader_header "${CMAKE_CURRENT_BINARY_DIR}/fixture_shaders.h")
set(_shader_sources "")
foreach(shader ${_shaders})
    list(APPEND _shader_sources "${_shader_dir}/${shader}.gxp")
endforeach()
add_custom_command(
    OUTPUT "${_shader_header}"
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${_shader_header} -DSHADER_DIR=${_shader_dir} "-DSHADERS=${_shaders}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake"
    DEPENDS ${_shader_sources} "${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake"
    COMMENT "Embedding GXM surface fixture shaders"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/main.o"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.elf"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.velf"
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" -O1 -I "${CMAKE_CURRENT_BINARY_DIR}"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/main.c" -o main.o
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" main.o -Wl,-q
            -lSceGxm_stub -lSceCommonDialog_stub -o fixture.elf
    COMMAND "${VITASDK}/bin/vita-elf-create" fixture.elf fixture.velf
    COMMAND "${VITASDK}/bin/vita-make-fself" fixture.velf eboot.bin
    DEPENDS main.c "${_shader_header}" "${VITASDK}/bin/arm-vita-eabi-gcc"
            "${VITASDK}/bin/vita-elf-create" "${VITASDK}/bin/vita-make-fself"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Building genuine VitaSDK GXM surface fixture"
    VERBATIM)
add_custom_target(vita3k_vita_gxm_surface_fixture ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")
