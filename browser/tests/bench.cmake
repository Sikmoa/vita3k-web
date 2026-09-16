# M14c benchmark harness (Node): the short display fixture through the real
# runtime with ASYNCIFY (vblank waits suspend through emscripten_sleep) and
# NODERAWFS, on both CPU backends, with identical sources and flag contexts.
#
# Included from browser/CMakeLists.txt right after the display fixture
# subdirectory (the run targets need its short eboot) and deliberately before
# the later add_compile_options(-O2 ...) line, so both bench modules share the
# same effective optimization flags as the existing vita3k_web_bench target.
#
# Nothing here touches wasm_jit_cpu.*, emit_wasm.* or vita_runtime.cpp: the
# bench drives the real entrypoint (vita3k_web_run_vita) exactly the way the
# browser Worker does, through browser/tests/display_bench_node.mjs.
#
# Targets (none are in the default build; the short eboot is never staged
# into dist):
#   vita3k_display_bench_node      interpreter bench module (Node, ASYNCIFY)
#   vita3k_display_bench_jit_node  JIT bench module (Node, ASYNCIFY); only
#                                  when VITA3K_WEB_JIT_TESTS=ON provided the
#                                  vita3k_wasm_jit target
#   vita3k_display_bench_interp    builds + runs the interpreter bench
#   vita3k_display_bench_jit       builds + runs the JIT bench
#
# Run (from the repository root, build dir build/web):
#   cmake --build build/web --target vita3k_display_bench_interp
#   cmake --build build/web --target vita3k_display_bench_jit

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "browser/tests/bench.cmake requires Emscripten")
endif()
if(NOT TARGET vita3k_vita_display_fixture_short)
    message(FATAL_ERROR "browser/tests/bench.cmake requires the display fixture (VITA3K_WEB_DISPLAY_FIXTURE=ON)")
endif()

find_program(VITA3K_NODE_EXECUTABLE node)

# One flag set for both backends. The only deltas between the two targets are
# the VITA3K_USE_WASM_JIT compile definition and the vita3k_wasm_jit library
# (whose INTERFACE link options - table growth, exported table, wasm table
# entry helper - propagate exactly as they do for vita3k_web_jit).
set(VITA3K_DISPLAY_BENCH_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vita_runtime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vita_display_bridge.cpp")
set(VITA3K_DISPLAY_BENCH_LINK_OPTIONS
    --no-entry
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=node
    -sASYNCIFY=1
    -sNODERAWFS=1
    ${VITA3K_WEB_GROWTH_LINK_OPTION}
    ${VITA3K_WEB_INITIAL_MEMORY_LINK_OPTION}
    -sSTACK_SIZE=1048576
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_fflush','_vita3k_web_alloc_input','_vita3k_web_free_input','_vita3k_web_run_vita','_vita3k_web_last_run_instructions','_vita3k_web_last_frame_instructions','_vita3k_web_set_fast_vblank']")

add_executable(vita3k_display_bench_node EXCLUDE_FROM_ALL
    ${VITA3K_DISPLAY_BENCH_SOURCES})
target_link_libraries(vita3k_display_bench_node PRIVATE vita3k_web_runtime_hle)
target_link_options(vita3k_display_bench_node PRIVATE
    ${VITA3K_DISPLAY_BENCH_LINK_OPTIONS})

if(TARGET vita3k_wasm_jit)
    add_executable(vita3k_display_bench_jit_node EXCLUDE_FROM_ALL
        ${VITA3K_DISPLAY_BENCH_SOURCES})
    target_compile_definitions(vita3k_display_bench_jit_node PRIVATE VITA3K_USE_WASM_JIT=1)
    target_link_libraries(vita3k_display_bench_jit_node PRIVATE
        vita3k_web_runtime_hle vita3k_wasm_jit)
    target_link_options(vita3k_display_bench_jit_node PRIVATE
        ${VITA3K_DISPLAY_BENCH_LINK_OPTIONS})
endif()

# Run targets: build the module and the short fixture, then execute the Node
# driver. The driver is the single place where the benchmark's metrics are
# computed; these targets only wire up inputs and working directory.
if(VITA3K_NODE_EXECUTABLE)
    set(VITA3K_DISPLAY_BENCH_SHORT_EBOOT
        "${CMAKE_BINARY_DIR}/browser/tests/vita_display_fixture/eboot-short.bin")
    add_custom_target(vita3k_display_bench_interp
        COMMAND "${VITA3K_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/display_bench_node.mjs"
            "${CMAKE_CURRENT_BINARY_DIR}/vita3k_display_bench_node.js"
            "${VITA3K_DISPLAY_BENCH_SHORT_EBOOT}"
            "interpreter"
        DEPENDS vita3k_display_bench_node vita3k_vita_display_fixture_short
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running interpreter display benchmark (short fixture, ASYNCIFY)"
        VERBATIM)
    if(TARGET vita3k_display_bench_jit_node)
        add_custom_target(vita3k_display_bench_jit
            COMMAND "${VITA3K_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/display_bench_node.mjs"
                "${CMAKE_CURRENT_BINARY_DIR}/vita3k_display_bench_jit_node.js"
                "${VITA3K_DISPLAY_BENCH_SHORT_EBOOT}"
                "jit"
            DEPENDS vita3k_display_bench_jit_node vita3k_vita_display_fixture_short
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Running JIT display benchmark (short fixture, ASYNCIFY)"
            VERBATIM)
    endif()
else()
    message(STATUS "node not found: display bench run targets omitted (modules still buildable)")
endif()
