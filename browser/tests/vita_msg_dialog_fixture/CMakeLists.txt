# Genuine VitaSDK message-dialog fixture (browser/tests/msg_dialog_chromium.mjs),
# built like vita_display_fixture: the SDK's default crt0, libc and linker
# script, and vita-elf-create + vita-make-fself, output in the binary directory.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vita_msg_dialog_fixture LANGUAGES NONE)
endif()

set(VITASDK "$ENV{VITASDK}" CACHE PATH "VitaSDK root (containing bin/arm-vita-eabi-gcc)")
foreach(tool arm-vita-eabi-gcc vita-elf-create vita-make-fself)
    if(NOT EXISTS "${VITASDK}/bin/${tool}")
        message(FATAL_ERROR "Set VITASDK to a VitaSDK installation; missing ${VITASDK}/bin/${tool}")
    endif()
endforeach()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/main.o"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.elf"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.velf"
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" -c "${CMAKE_CURRENT_SOURCE_DIR}/main.c" -o main.o
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" main.o -Wl,-q
            -lSceCommonDialog_stub -lSceDisplay_stub -o fixture.elf
    COMMAND "${VITASDK}/bin/vita-elf-create" fixture.elf fixture.velf
    COMMAND "${VITASDK}/bin/vita-make-fself" fixture.velf eboot.bin
    DEPENDS main.c "${VITASDK}/bin/arm-vita-eabi-gcc"
            "${VITASDK}/bin/vita-elf-create" "${VITASDK}/bin/vita-make-fself"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Building genuine VitaSDK message-dialog fixture"
    VERBATIM)
add_custom_target(vita3k_vita_msg_dialog_fixture
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")
