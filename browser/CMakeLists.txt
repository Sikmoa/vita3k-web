cmake_minimum_required(VERSION 3.22)

# M1 deliberately starts as a host-independent lifecycle probe. The full
# emulator graph is added incrementally after each browser-facing boundary is
# tested; native Vita3K targets remain in the normal CMake path above.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_core.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_hle.cmake)

find_program(VITA3K_ARM_CLANG NAMES clang REQUIRED)
find_program(VITA3K_ARM_LLD NAMES ld.lld-19 ld.lld REQUIRED)

set(VITA3K_GUEST_PROBE_ELF "${CMAKE_CURRENT_BINARY_DIR}/guest_probe.elf")
add_custom_command(
    OUTPUT "${VITA3K_GUEST_PROBE_ELF}"
    COMMAND "${VITA3K_ARM_CLANG}"
        --target=armv7-none-eabi
        -mthumb
        -nostdlib
        "-fuse-ld=${VITA3K_ARM_LLD}"
        "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.ld"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.S"
        -o "${VITA3K_GUEST_PROBE_ELF}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.ld"
    COMMENT "Building ARM guest ELF probe"
    VERBATIM
)
add_custom_target(vita3k_guest_probe_elf ALL DEPENDS "${VITA3K_GUEST_PROBE_ELF}")

add_executable(vita3k_web
    src/main.cpp
    src/vita_runtime.cpp
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
)

add_dependencies(vita3k_web vita3k_guest_probe_elf)
target_compile_definitions(vita3k_web PRIVATE VITA3K_WEB=1)
target_link_libraries(vita3k_web PRIVATE vita3k_web_runtime_hle)

add_executable(vita3k_web_memory_tests
    src/memory.cpp
    tests/memory_tests.cpp
)

add_executable(vita3k_web_interpreter_tests
    src/memory.cpp
    src/interpreter.cpp
    tests/interpreter_tests.cpp
)

add_executable(vita3k_web_guest_tests
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
    tests/guest_tests.cpp
)

add_executable(vita3k_web_generated_elf_tests
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
    tests/guest_probe/guest_probe_tests.cpp
)
add_dependencies(vita3k_web_generated_elf_tests vita3k_guest_probe_elf)
if(EMSCRIPTEN)
    target_link_options(vita3k_web_generated_elf_tests PRIVATE -sNODERAWFS=1)
endif()

if(NOT EMSCRIPTEN)
    enable_testing()
    add_test(NAME vita3k_web_memory COMMAND vita3k_web_memory_tests)
    add_test(NAME vita3k_web_interpreter COMMAND vita3k_web_interpreter_tests)
    add_test(NAME vita3k_web_guest COMMAND vita3k_web_guest_tests)
    add_test(NAME vita3k_web_generated_elf COMMAND vita3k_web_generated_elf_tests "${VITA3K_GUEST_PROBE_ELF}")
endif()

target_link_options(vita3k_web PRIVATE
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_ES6=1"
    "SHELL:-sEXPORT_NAME=Vita3KWeb"
    "SHELL:-sENVIRONMENT=web,worker"
    "SHELL:-sEXIT_RUNTIME=0"
    "SHELL:-sNO_EXIT_RUNTIME=1"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sINITIAL_MEMORY=67108864"
    "SHELL:-sSTACK_SIZE=1048576"
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
)

set_target_properties(vita3k_web PROPERTIES
    OUTPUT_NAME "vita3k_web"
    SUFFIX ".js"
)

add_custom_command(TARGET vita3k_web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VITA3K_GUEST_PROBE_ELF}"
        "${CMAKE_BINARY_DIR}/dist/guest.elf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
        "${CMAKE_BINARY_DIR}/dist/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/worker.js"
        "${CMAKE_BINARY_DIR}/dist/worker.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/capabilities.js"
        "${CMAKE_BINARY_DIR}/dist/capabilities.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/storage.js"
        "${CMAKE_BINARY_DIR}/dist/storage.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/webgpu.js"
        "${CMAKE_BINARY_DIR}/dist/webgpu.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/audio_input.js"
        "${CMAKE_BINARY_DIR}/dist/audio_input.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:vita3k_web>"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web.wasm"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.wasm"
    COMMENT "Staging Vita3K browser bootstrap"
)
