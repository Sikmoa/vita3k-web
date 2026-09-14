# Genuine VitaSDK CPU-rendered display fixture. Mirrors the
# vita_homebrew_fixture recipe: the real toolchain, the SDK's default
# crt0/libc/linker script, and vita-elf-create + vita-make-fself, with every
# generated artifact in the binary directory (nothing SDK-generated committed).
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vita_display_fixture LANGUAGES NONE)
endif()

set(VITASDK "$ENV{VITASDK}" CACHE PATH "VitaSDK root (containing bin/arm-vita-eabi-gcc)")
foreach(tool arm-vita-eabi-gcc vita-elf-create vita-make-fself)
    if(NOT EXISTS "${VITASDK}/bin/${tool}")
        message(FATAL_ERROR "Set VITASDK to a VitaSDK installation; missing ${VITASDK}/bin/${tool}")
    endif()
endforeach()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Build the fixture out of source (cmake -S ... -B build/vita-display-fixture)")
endif()

# Optional: render fewer frames for slow interpreter bring-up runs
# (default in main.c is 600, ~10 s at 60 fps).
set(VITA_DISPLAY_FRAME_COUNT "" CACHE STRING "Frames rendered before exit (default 600)")
set(_display_fixture_c_flags)
if(VITA_DISPLAY_FRAME_COUNT)
    set(_display_fixture_c_flags "-DVITA_DISPLAY_FRAME_COUNT=${VITA_DISPLAY_FRAME_COUNT}")
endif()

# Keep the SDK's default crt0, libc, import stubs and linker script. No
# hand-written entry point, -nostdlib, generated instruction stream, or
# replacement imports. The single deliberate delta from the exit-42 fixture
# recipe is linking the SceDisplay stub library: this fixture actually calls
# sceDisplaySetFrameBuf / sceDisplayWaitVblankStart.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/main.o"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.elf"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture.velf"
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc"
            ${_display_fixture_c_flags}
            -c "${CMAKE_CURRENT_SOURCE_DIR}/main.c" -o main.o
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" main.o -Wl,-q -lSceDisplay_stub -o fixture.elf
    COMMAND "${VITASDK}/bin/vita-elf-create" fixture.elf fixture.velf
    COMMAND "${VITASDK}/bin/vita-make-fself" fixture.velf eboot.bin
    DEPENDS main.c "${VITASDK}/bin/arm-vita-eabi-gcc"
            "${VITASDK}/bin/vita-elf-create" "${VITASDK}/bin/vita-make-fself"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Building genuine VitaSDK display fixture with default crt0"
    VERBATIM)
add_custom_target(vita3k_vita_display_fixture
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")

# Short variant for benchmark harnesses (M14c): same source, same toolchain,
# fewer frames. It uses its own intermediate/output names so it can never
# clobber or be confused with the default 600-frame eboot above, and it is
# only ever produced in the build directory — nothing stages it into dist.
set(VITA_DISPLAY_SHORT_FRAME_COUNT "60" CACHE STRING
    "Frames rendered by the short (bench-only) display fixture eboot")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot-short.bin"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/main-short.o"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture-short.elf"
               "${CMAKE_CURRENT_BINARY_DIR}/fixture-short.velf"
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc"
            "-DVITA_DISPLAY_FRAME_COUNT=${VITA_DISPLAY_SHORT_FRAME_COUNT}"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/main.c" -o main-short.o
    COMMAND "${VITASDK}/bin/arm-vita-eabi-gcc" main-short.o -Wl,-q -lSceDisplay_stub -o fixture-short.elf
    COMMAND "${VITASDK}/bin/vita-elf-create" fixture-short.elf fixture-short.velf
    COMMAND "${VITASDK}/bin/vita-make-fself" fixture-short.velf eboot-short.bin
    DEPENDS main.c "${VITASDK}/bin/arm-vita-eabi-gcc"
            "${VITASDK}/bin/vita-elf-create" "${VITASDK}/bin/vita-make-fself"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Building short (${VITA_DISPLAY_SHORT_FRAME_COUNT}-frame) display fixture (bench only)"
    VERBATIM)
add_custom_target(vita3k_vita_display_fixture_short
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot-short.bin")
