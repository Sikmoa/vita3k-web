cmake_minimum_required(VERSION 3.22)

# M1 deliberately starts as a host-independent lifecycle probe. The full
# emulator graph is added incrementally after each browser-facing boundary is
# tested; native Vita3K targets remain in the normal CMake path above.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_memory.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_core.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_hle.cmake)

# Experimental M14 tests only; the existing application keeps InterpreterCPU.
option(VITA3K_WEB_JIT_TESTS "Build Dynarmic-IR to Wasm JIT prototype tests" OFF)
if(VITA3K_WEB_JIT_TESTS)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "VITA3K_WEB_JIT_TESTS requires Emscripten")
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_wasmjit.cmake)
endif()

# Genuine VitaSDK display fixture (CPU-rendered animated framebuffer).
option(VITA3K_WEB_DISPLAY_FIXTURE "Build the genuine VitaSDK display fixture" ON)
if(VITA3K_WEB_DISPLAY_FIXTURE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests/vita_display_fixture)
    # M14c benchmark harness: Node bench modules/run targets over the short
    # fixture. Included before the -O2 add_compile_options line below so the
    # interpreter and JIT bench modules share one effective flag context
    # (the same one the existing vita3k_web_bench target gets).
    include(${CMAKE_CURRENT_SOURCE_DIR}/tests/bench.cmake)
endif()

# Node-runnable interpreter benchmark over the genuine exit-42 fixture. It is
# deliberately built WITHOUT ASYNCIFY so the numbers measure pure interpreter
# throughput; the browser target itself needs ASYNCIFY for cooperative waits.
if(EMSCRIPTEN)
    add_executable(vita3k_web_bench
        src/vita_runtime.cpp
        src/vita_display_bridge.cpp
        tests/vita_bench_main.cpp
    )
    target_link_libraries(vita3k_web_bench PRIVATE vita3k_web_runtime_hle)
    target_link_options(vita3k_web_bench PRIVATE
        ${VITA3K_WEB_GROWTH_LINK_OPTION}
        "SHELL:-sSTACK_SIZE=1048576"
        "SHELL:-sNODERAWFS=1"
    )
endif()

find_program(VITA3K_ARM_CLANG NAMES clang REQUIRED)
find_program(VITA3K_ARM_LLD NAMES ld.lld-19 ld.lld REQUIRED)

# The browser runtime is an emulated machine: debug (-g, no optimization) Wasm
# runs the interpreter ~100x too slow for frame-paced guest loops. Default the
# browser target to optimized codegen while keeping debug info available via
# a separate build directory configured with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND EMSCRIPTEN)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options($<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:MinSizeRel>:-Os>)

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
    src/vita_display_bridge.cpp
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
)

add_dependencies(vita3k_web vita3k_guest_probe_elf)
target_compile_definitions(vita3k_web PRIVATE VITA3K_WEB=1)
target_link_libraries(vita3k_web PRIVATE vita3k_web_runtime_hle)
# The POST_BUILD staging step copies the fixture eboot into dist, so the
# fixture must be built before the browser module links/stages.
if(TARGET vita3k_vita_display_fixture)
    add_dependencies(vita3k_web vita3k_vita_display_fixture)
endif()

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
    ${VITA3K_WEB_MEMORY_LINK_OPTIONS}
    "SHELL:-sSTACK_SIZE=1048576"
    "SHELL:-sASYNCIFY"
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_vita3k_web_run_vita','_vita3k_web_last_run_instructions']"
)

set_target_properties(vita3k_web PROPERTIES
    OUTPUT_NAME "vita3k_web"
    SUFFIX ".js"
)

# Staging of non-link inputs is driven by output-dependent custom commands,
# not a POST_BUILD step: a POST_BUILD only runs when the target relinks, so
# edits to the web assets or the fixture (not link inputs) would never reach
# dist. The module's own js/wasm outputs are staged by POST_BUILD below,
# which runs exactly when those files change.
set(_staged_assets
    "${VITA3K_GUEST_PROBE_ELF}|${CMAKE_BINARY_DIR}/dist/guest.elf"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html|${CMAKE_BINARY_DIR}/dist/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/display.html|${CMAKE_BINARY_DIR}/dist/display.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/display.js|${CMAKE_BINARY_DIR}/dist/display.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/worker.js|${CMAKE_BINARY_DIR}/dist/worker.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/capabilities.js|${CMAKE_BINARY_DIR}/dist/capabilities.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/storage.js|${CMAKE_BINARY_DIR}/dist/storage.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/webgpu.js|${CMAKE_BINARY_DIR}/dist/webgpu.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/gxm_renderer.js|${CMAKE_BINARY_DIR}/dist/gxm_renderer.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/audio_input.js|${CMAKE_BINARY_DIR}/dist/audio_input.js"
)
set(_stage_outputs "")
foreach(_pair IN LISTS _staged_assets)
    string(REGEX REPLACE "^([^|]+)\\|(.+)$" "\\1;\\2" _paths "${_pair}")
    list(GET _paths 0 _src)
    list(GET _paths 1 _dst)
    add_custom_command(
        OUTPUT "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
        DEPENDS "${_src}"
        COMMENT "Staging ${_dst}"
    )
    list(APPEND _stage_outputs "${_dst}")
endforeach()
add_custom_target(vita3k_web_dist_stage DEPENDS ${_stage_outputs})
add_custom_command(TARGET vita3k_web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web.js"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web.wasm"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.wasm"
)

# The fixture eboot is staged by its own output-dependent rule: a POST_BUILD
# step on vita3k_web would not re-run when only the fixture changes (the
# fixture is not a link input), so a rebuilt fixture would never reach dist.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/dist/display-eboot.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/tests/vita_display_fixture/eboot.bin"
        "${CMAKE_BINARY_DIR}/dist/display-eboot.bin"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/tests/vita_display_fixture/eboot.bin"
    COMMENT "Staging genuine Vita display fixture"
)
add_custom_target(vita3k_web_fixture_stage
    DEPENDS "${CMAKE_BINARY_DIR}/dist/display-eboot.bin"
)
add_dependencies(vita3k_web_dist_stage vita3k_web_fixture_stage)
add_dependencies(vita3k_web vita3k_web_dist_stage)

vita3k_web_finalize_memory64("${CMAKE_CURRENT_SOURCE_DIR}")
