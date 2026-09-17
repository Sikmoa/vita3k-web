# M14 is opt-in and does not replace the working display application's CPU.
include(${CMAKE_CURRENT_LIST_DIR}/runtime_dynarmic_frontend.cmake)
add_library(vita3k_wasm_jit STATIC
    "${VITA_ROOT}/cpu/src/wasm_jit_cpu.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/emit_wasm.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/fp64.cpp"
)
target_link_libraries(vita3k_wasm_jit PUBLIC vita3k_dynarmic_frontend vita3k_web_runtime_core)
target_include_directories(vita3k_wasm_jit PRIVATE "${VITA_ROOT}/cpu/src")
target_link_options(vita3k_wasm_jit INTERFACE
    ${VITA3K_WEB_GROWTH_LINK_OPTION} -sALLOW_TABLE_GROWTH=1 -Wl,--export-table
    -sSTACK_SIZE=1048576 "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$setWasmTableEntry','$getWasmTableEntry']"
)

# Separate fixture-launch targets: opting into tests does not switch the
# already-working interpreter application. The Worker selects this module only
# when created with ?backend=jit.
add_executable(vita3k_web_jit
    src/main.cpp src/vita_runtime.cpp src/vita_display_bridge.cpp
    src/memory.cpp src/interpreter.cpp src/guest.cpp)
target_compile_definitions(vita3k_web_jit PRIVATE VITA3K_WEB=1 VITA3K_USE_WASM_JIT=1)
target_link_libraries(vita3k_web_jit PRIVATE vita3k_web_runtime_hle vita3k_wasm_jit)
target_link_options(vita3k_web_jit PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=Vita3KWebJit
    -sENVIRONMENT=web,worker -sNO_EXIT_RUNTIME=1 -sASYNCIFY=1
    ${VITA3K_WEB_INITIAL_MEMORY_LINK_OPTION} "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']")
add_custom_command(TARGET vita3k_web_jit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web_jit.js" "${CMAKE_BINARY_DIR}/dist/vita3k_web_jit.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web_jit.wasm" "${CMAKE_BINARY_DIR}/dist/vita3k_web_jit.wasm")

add_executable(vita3k_jit_fixture_node
    src/vita_runtime.cpp src/vita_display_bridge.cpp tests/vita_bench_main.cpp)
target_compile_definitions(vita3k_jit_fixture_node PRIVATE VITA3K_USE_WASM_JIT=1)
target_link_libraries(vita3k_jit_fixture_node PRIVATE vita3k_web_runtime_hle vita3k_wasm_jit)
target_link_options(vita3k_jit_fixture_node PRIVATE -sNODERAWFS=1)

add_executable(vita3k_jit_bench tests/jit_bench.cpp)
target_link_libraries(vita3k_jit_bench PRIVATE vita3k_wasm_jit)

add_executable(vita3k_jit_tests_node tests/wasm_jit_tests.cpp)
target_link_libraries(vita3k_jit_tests_node PRIVATE vita3k_wasm_jit)

add_executable(vita3k_jit_tests tests/wasm_jit_tests.cpp)
target_compile_definitions(vita3k_jit_tests PRIVATE VITA3K_JIT_TEST_NO_MAIN=1)
target_link_libraries(vita3k_jit_tests PRIVATE vita3k_wasm_jit)
target_link_options(vita3k_jit_tests PRIVATE
    --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=Vita3KJitTests
    -sENVIRONMENT=web,worker "-sEXPORTED_FUNCTIONS=['_vita3k_web_jit_tests']"
)

# Backend integration test: this TU includes wasm_jit_cpu.cpp directly to
# exercise its anonymous-namespace checked-memory helpers, so it links the
# frontend/core but NOT the vita3k_wasm_jit library (no duplicate symbols).
add_executable(vita3k_jit_backend_test_node "${VITA_ROOT}/cpu/tests/wasmjit_backend_test.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/emit_wasm.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/fp64.cpp")
target_include_directories(vita3k_jit_backend_test_node PRIVATE "${VITA_ROOT}/cpu/src")
target_link_libraries(vita3k_jit_backend_test_node PRIVATE vita3k_dynarmic_frontend vita3k_web_runtime_core)
target_link_options(vita3k_jit_backend_test_node PRIVATE
    ${VITA3K_WEB_GROWTH_LINK_OPTION} -sALLOW_TABLE_GROWTH=1 -Wl,--export-table
    -sSTACK_SIZE=1048576 "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$setWasmTableEntry','$getWasmTableEntry']")
