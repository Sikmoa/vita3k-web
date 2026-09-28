#!/usr/bin/env bash
# Build the exact arithmetic module used by wasmjit_emitter_test.mjs.
set -euo pipefail
cd "$(dirname "$0")/../../.."
output="${1:-/tmp/wasmjit-fp-helper.wasm}"
cmake -DDYNARMIC_ROOT="$PWD/external/dynarmic" -DFUSED_OUTPUT="${output}.fused.cpp" -P vita3k/cpu/src/wasmjit/prepare_fused.cmake
em++ -std=c++20 -O2 -DFMT_CONSTEVAL= -Wno-deprecated-literal-operator \
    -Iexternal/dynarmic/src -Iexternal/dynarmic/externals/mcl/include -Iexternal/dynarmic/externals/fmt/include \
    vita3k/cpu/tests/wasmjit_fp_helper_exports.cpp vita3k/cpu/src/wasmjit/fp64.cpp \
    external/dynarmic/src/dynarmic/common/fp/op/{FPConvert,FPMulAdd,FPRecipEstimate,FPRecipStepFused,FPRSqrtEstimate,FPRSqrtStepFused,FPRoundInt,FPToFixed}.cpp \
    "${output}.fused.cpp" external/dynarmic/src/dynarmic/common/fp/{process_exception,process_nan,unpacked}.cpp \
    external/dynarmic/src/dynarmic/common/{u128,math_util}.cpp \
    external/dynarmic/externals/mcl/src/assert.cpp external/dynarmic/externals/fmt/src/format.cc \
    -sSTANDALONE_WASM -sEXPORTED_FUNCTIONS='["_extended_fp","_extended_flags"]' --no-entry -o "$output"
