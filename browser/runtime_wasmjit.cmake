# M14 is opt-in and does not replace the working display application's CPU.
include(${CMAKE_CURRENT_LIST_DIR}/runtime_dynarmic_frontend.cmake)
add_library(vita3k_wasm_jit STATIC
    "${VITA_ROOT}/cpu/src/wasm_jit_cpu.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/emit_wasm.cpp"
)
target_link_libraries(vita3k_wasm_jit PUBLIC vita3k_dynarmic_frontend vita3k_web_runtime_core)
target_include_directories(vita3k_wasm_jit PRIVATE "${VITA_ROOT}/cpu/src")
target_link_options(vita3k_wasm_jit INTERFACE
    -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -Wl,--export-table
    -sSTACK_SIZE=1048576 "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$setWasmTableEntry']"
)

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
