// Assemble/disassemble with VitaSDK; this file is a verifier's encoding oracle.
// The C++ harness independently constructs the full register/width matrix.
.syntax unified
.arch armv7-a
.fpu neon
.text
.arm
.global neon_integer_arm
neon_integer_arm:
    vadd.i8 q15, q15, q1
    vsub.i64 d31, d2, d31
    vmul.i16 q15, q15, q1
    vmla.i32 d31, d31, d2
    vmls.i8 q15, q1, q15
    vneg.s32 q15, q15
    vmul.i16 q15, q15, d3[3]
    vmla.i32 d31, d31, d3[1]
    vmls.i16 d31, d31, d3[3]
    svc #0
.thumb
.thumb_func
.global neon_integer_thumb
neon_integer_thumb:
    vadd.i8 q15, q15, q1
    vsub.i64 d31, d2, d31
    vmul.i16 q15, q15, q1
    vmla.i32 d31, d31, d2
    vmls.i8 q15, q1, q15
    vneg.s32 q15, q15
    vmul.i16 q15, q15, d3[3]
    vmla.i32 d31, d31, d3[1]
    vmls.i16 d31, d31, d3[3]
    svc #0
