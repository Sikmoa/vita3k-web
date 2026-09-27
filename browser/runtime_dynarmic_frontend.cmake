# Portable Dynarmic A32 decoder/translator and native IR, NOT the native JIT.
# Include after runtime_core.cmake. Do not add_subdirectory(dynarmic): that pulls
# in host architecture detection, executable memory and native code generators.
include_guard(DIRECTORY)
if(NOT TARGET fmt::fmt OR NOT TARGET vita3k_web_runtime_core)
    message(FATAL_ERROR "runtime_dynarmic_frontend.cmake requires runtime_core.cmake first")
endif()

set(_dynarmic_root "${EXT_ROOT}/dynarmic")
set(_dynarmic_src "${_dynarmic_root}/src/dynarmic")
if(NOT TARGET merry::mcl)
    set(MCL_INSTALL OFF CACHE BOOL "" FORCE)
    set(MCL_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_dynarmic_root}/externals/mcl"
        "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/mcl" EXCLUDE_FROM_ALL)
endif()

# Upstream IR also exposes four native CallHostFunction overloads. They assume
# sizeof(function pointer) == 8, which fails on wasm32 even though A32 Translate
# never calls them. Generate a build-local copy that explicitly rejects this
# native-only escape hatch. Do NOT widen Wasm table indices into fake host
# addresses, weaken mcl::bit_cast, or edit the vendored checkout.
set(_dynarmic_ir_emitter "${_dynarmic_src}/ir/ir_emitter.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dynarmic_ir_emitter}")
file(READ "${_dynarmic_ir_emitter}" _dynarmic_ir_text)
set(_dynarmic_host_call_pattern "Inst\\(Opcode::CallHostFunction, Imm64\\(mcl::bit_cast<u64>\\(fn\\)\\), [^\n]*\\);")
# Match only the pointer expression when counting: CMake list separators in
# complete C++ statements would otherwise turn four matches into eight entries.
string(REGEX MATCHALL "mcl::bit_cast<u64>\\(fn\\)" _dynarmic_host_calls "${_dynarmic_ir_text}")
list(LENGTH _dynarmic_host_calls _dynarmic_host_call_count)
if(NOT _dynarmic_host_call_count EQUAL 4)
    message(FATAL_ERROR "Dynarmic CallHostFunction changed; review the portable frontend adaptation")
endif()
string(REGEX REPLACE "${_dynarmic_host_call_pattern}"
    "throw std::logic_error(\"native CallHostFunction is unavailable in vita3k_dynarmic_frontend\");"
    _dynarmic_ir_text "${_dynarmic_ir_text}")
string(FIND "${_dynarmic_ir_text}" "mcl::bit_cast<u64>(fn)" _dynarmic_unpatched_host_call)
if(NOT _dynarmic_unpatched_host_call EQUAL -1)
    message(FATAL_ERROR "Dynarmic native host-call adaptation was incomplete")
endif()
set(_dynarmic_portable_ir "${CMAKE_CURRENT_BINARY_DIR}/dynarmic-frontend/ir_emitter.cpp")
file(GENERATE OUTPUT "${_dynarmic_portable_ir}" CONTENT "#include <stdexcept>\n${_dynarmic_ir_text}")

# All visitor implementations are required by the upstream decoder tables,
# including VFP/ASIMD. This glob is deliberately confined to the A32 frontend.
file(GLOB _dynarmic_a32_impl CONFIGURE_DEPENDS
    "${_dynarmic_src}/frontend/A32/translate/impl/*.cpp")
add_library(vita3k_dynarmic_frontend STATIC
    ${_dynarmic_a32_impl}
    "${_dynarmic_src}/frontend/A32/translate/a32_translate.cpp"
    "${_dynarmic_src}/frontend/A32/translate/translate_arm.cpp"
    "${_dynarmic_src}/frontend/A32/translate/translate_thumb.cpp"
    "${_dynarmic_src}/frontend/A32/translate/conditional_state.cpp"
    "${_dynarmic_src}/frontend/A32/a32_ir_emitter.cpp"
    "${_dynarmic_src}/frontend/A32/a32_location_descriptor.cpp"
    "${_dynarmic_src}/frontend/A32/a32_types.cpp"
    # Core IR diagnostics use A64 register/condition names, not A64 translation.
    "${_dynarmic_src}/frontend/A64/a64_types.cpp"
    "${_dynarmic_src}/frontend/imm.cpp"
    "${_dynarmic_src}/common/memory_pool.cpp"
    "${_dynarmic_src}/ir/basic_block.cpp"
    "${_dynarmic_portable_ir}"
    "${_dynarmic_src}/ir/location_descriptor.cpp"
    "${_dynarmic_src}/ir/microinstruction.cpp"
    "${_dynarmic_src}/ir/opcodes.cpp"
    "${_dynarmic_src}/ir/type.cpp"
    "${_dynarmic_src}/ir/value.cpp"
    "${_dynarmic_src}/ir/opt/naming_pass.cpp"
    "${_dynarmic_src}/ir/opt/verification_pass.cpp"
    # Portable FP helpers: the wasmjit fp64 host helper delegates the ARM
    # vector RECPE/VRECPS/VRSQRTE/VRSQRTS estimates to these implementations instead of
    # re-deriving the algorithms (correct-by-construction, one copy).
    "${_dynarmic_src}/common/fp/fused.cpp"
    "${_dynarmic_src}/common/fp/process_exception.cpp"
    "${_dynarmic_src}/common/fp/process_nan.cpp"
    "${_dynarmic_src}/common/fp/unpacked.cpp"
    "${_dynarmic_src}/common/fp/op/FPRecipEstimate.cpp"
    "${_dynarmic_src}/common/fp/op/FPRecipStepFused.cpp"
    "${_dynarmic_src}/common/fp/op/FPRSqrtEstimate.cpp"
    "${_dynarmic_src}/common/fp/op/FPRSqrtStepFused.cpp"
    "${_dynarmic_src}/common/fp/op/FPToFixed.cpp"
    # Oracle for the emitted FPHalfToSingle lowering in the backend tests.
    "${_dynarmic_src}/common/fp/op/FPConvert.cpp"
    "${_dynarmic_src}/common/u128.cpp"
    "${_dynarmic_src}/common/math_util.cpp"
    "${VITA_ROOT}/cpu/src/wasmjit/frontend.cpp"
)
target_compile_features(vita3k_dynarmic_frontend PUBLIC cxx_std_20)
target_include_directories(vita3k_dynarmic_frontend PUBLIC
    "${_dynarmic_root}/src"
    "${VITA_ROOT}/cpu/src"
    "${EXT_ROOT}/boost"
    ${BOOST_HEADER_DIRS} ${BOOST_NUMERIC_HEADER_DIRS}
    PRIVATE "${VITA_ROOT}/mem/include"
)
target_link_libraries(vita3k_dynarmic_frontend PUBLIC fmt::fmt merry::mcl)
# Fetch faults propagate out of Translate. Consumers must enable exceptions too.
if(EMSCRIPTEN)
    target_compile_options(vita3k_dynarmic_frontend PUBLIC -fexceptions)
    target_link_options(vita3k_dynarmic_frontend INTERFACE -fexceptions)
endif()
# Deliberately no runtime-core dependency: the runtime can link this library;
# the final consumer supplies mem_fetch. No DYNARMIC_IGNORE_ASSERTS workaround.

option(VITA3K_WEB_JIT_IR_PROBE "Build the standalone Dynarmic translation probe" OFF)
if(VITA3K_WEB_JIT_IR_PROBE)
    add_executable(vita3k_web_jit_ir_probe "${CMAKE_CURRENT_LIST_DIR}/tests/jit_ir_probe.cpp")
    target_link_libraries(vita3k_web_jit_ir_probe PRIVATE
        vita3k_dynarmic_frontend vita3k_web_runtime_core)
    if(EMSCRIPTEN)
        target_link_options(vita3k_web_jit_ir_probe PRIVATE
            ${VITA3K_WEB_GROWTH_LINK_OPTION} -sSTACK_SIZE=1048576
            -sENVIRONMENT=node -sWASM_ASYNC_COMPILATION=0)
    endif()
endif()
