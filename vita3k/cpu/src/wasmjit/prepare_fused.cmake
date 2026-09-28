# Adapt the vendored portable FMA without modifying its submodule. When
# cancellation leaves only the low 64-bit limb, its binary point is still
# at bit 124, not bit 62. Upstream omits that exponent correction.
file(READ "${DYNARMIC_ROOT}/src/dynarmic/common/fp/fused.cpp" _fused)
set(_old "return FPUnpacked{result_sign, result_exponent, result.lower};")
set(_new "return FPUnpacked{result_sign, result_exponent - int(normalized_point_position), result.lower};")
string(FIND "${_fused}" "${_old}" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "Dynarmic FusedMulAdd changed; review cancellation exponent correction")
endif()
string(REPLACE "${_old}" "${_new}" _fused "${_fused}")
file(WRITE "${FUSED_OUTPUT}" "${_fused}")
