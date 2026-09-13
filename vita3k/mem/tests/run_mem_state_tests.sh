#!/usr/bin/env bash
# Standalone: compile production mem.cpp + allocator.cpp with restored vendored
# headers. No CPU/kernel/browser sources or substitute mem implementations.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${MEM_TEST_OUT:-/tmp/vita3k-mem-state-tests}
mkdir -p "$out"
cd "$root"
common=(-std=c++20 -O0 -g0 -Wall -Wextra -Wno-deprecated-declarations
    -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY
    -Ivita3k/mem/include -Ivita3k/util/include
    -Iexternal/boost -Iexternal/fmt/include -Iexternal/spdlog/include)
sources=(vita3k/mem/src/mem.cpp vita3k/mem/src/allocator.cpp vita3k/mem/tests/mem_state_tests.cpp)
case "${1:-all}" in
    native|all)
        "${CXX:-clang++}" "${common[@]}" -fsanitize=undefined -fno-sanitize-recover=all \
            "${sources[@]}" -pthread -o "$out/mem-state-native"
        "$out/mem-state-native"
        ;;
    wasm) ;;
    *) echo "usage: $0 [all|native|wasm]" >&2; exit 2 ;;
esac
case "${1:-all}" in
    wasm|all)
        "${EMXX:-em++}" "${common[@]}" -fexceptions "${sources[@]}" \
            -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=67108864 \
            -sABORTING_MALLOC=0 -sASSERTIONS=2 -sSTACK_SIZE=1048576 \
            -sENVIRONMENT=node -sSINGLE_FILE=1 -o "$out/mem-state-wasm.js"
        node "$out/mem-state-wasm.js"
        ;;
esac
