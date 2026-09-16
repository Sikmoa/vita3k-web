#!/usr/bin/env bash
# Standalone shader compiler only: no CMake, GUI, renderer, guest CPU or HLE build.
set -euo pipefail
cd "$(dirname "$0")/../.."
export TMPDIR="$PWD/.limbo_work/tmp"
mkdir -p "$TMPDIR"
mode=${1:-native}
[[ $mode == native || $mode == wasm ]] || { echo 'usage: build_gxp_compiler.sh [native|wasm]' >&2; exit 2; }
out="$PWD/.limbo_work/gxm/shader-$mode"
mkdir -p "$out"
sources=(browser/tests/gxp_compile.cpp vita3k/shader/src/*.cpp vita3k/shader/src/translator/*.cpp
  vita3k/gxm/src/gxp.cpp vita3k/gxm/src/attributes.cpp vita3k/gxm/src/color.cpp vita3k/gxm/src/textures.cpp
  external/glslang/SPIRV/SpvBuilder.cpp external/glslang/SPIRV/SpvPostProcess.cpp
  external/glslang/SPIRV/InReadableOrder.cpp external/glslang/SPIRV/Logger.cpp external/glslang/SPIRV/disassemble.cpp external/glslang/SPIRV/doc.cpp
  external/fmt/src/format.cc)
flags=(-std=c++23 -O0 -g0 -ffunction-sections -fdata-sections -DVITA3K_SHADER_SPIRV_ONLY
  -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_THREAD_ID -DSPDLOG_NO_TLS
  -I vita3k/shader/include -I vita3k/gxm/include -I vita3k/features/include
  -I vita3k/util/include -I vita3k/mem/include -I external/fmt/include
  -I external/spdlog/include -I external/boost -I external/glslang)
if [[ $mode == wasm ]]; then
  compiler=${EMXX:-em++}
  # Never use or modify the shared frozen cache. All generation is private.
  export EM_CACHE="$out/emcache" EM_FROZEN_CACHE=0 EMCC_CORES=1
  flags+=(-fexceptions)
  link=(-sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node,worker
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=268435456
    -sDISABLE_EXCEPTION_CATCHING=0 -sFILESYSTEM=0
    '-sEXPORTED_FUNCTIONS=["_malloc","_free","_gxp_compile","_gxp_output_data","_gxp_output_size","_gxp_error"]'
    '-sEXPORTED_RUNTIME_METHODS=["UTF8ToString"]' --no-entry)
  target="$out/gxp_compiler.mjs"
else
  compiler=${CXX:-c++}
  link=(-Wl,--gc-sections -pthread)
  target="$PWD/.limbo_work/gxm/gxp_compile"
fi
objects=()
# Sequential by construction; wrap the entire script in timeout -s KILL 240s.
for source in "${sources[@]}"; do
  object="$out/${source//\//_}.o"
  objects+=("$object")
  # Dependency files include headers so edits cannot silently reuse stale objects.
  if [[ -f $object && -f $object.d ]] && ! find "$source" vita3k/shader/include vita3k/gxm/include \
      vita3k/util/include external/glslang/SPIRV -type f -newer "$object" -print -quit | grep -q .; then
    continue
  fi
  echo "[$mode] $source"
  "$compiler" "${flags[@]}" -MMD -MF "$object.d" -c "$source" -o "$object"
done
"$compiler" "${flags[@]}" "${objects[@]}" "${link[@]}" -o "$target"
echo "Built $target"
