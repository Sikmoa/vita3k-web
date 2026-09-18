// Source-owned encodings for wasmjit_vectorfp_compare_tests.inc.
// Assemble with arm-vita-eabi-as; no game code/data is required.
.syntax unified
.arch armv7-a
.fpu neon
.text
.arm
arm_vector_fp_compare:
    vcgt.f32 d0, d5, d1
    vcge.f32 d0, d5, d1
    vceq.f32 d0, d5, d1
    vcgt.f32 q15, q15, q1
    vcge.f32 q15, q1, q15
    vceq.f32 q15, q15, q15
    vcgt.f32 d31, d31, #0
    vcge.f32 d31, d31, #0
    vceq.f32 d31, d31, #0
    vcle.f32 d31, d31, #0
    vclt.f32 d31, d31, #0
.thumb
thumb_vector_fp_compare:
    vcgt.f32 d0, d5, d1
    vcge.f32 d0, d5, d1
    vceq.f32 d0, d5, d1
    vcgt.f32 q15, q15, q1
    vcge.f32 q15, q1, q15
    vceq.f32 q15, q15, q15
    vcgt.f32 d31, d31, #0
    vcge.f32 d31, d31, #0
    vceq.f32 d31, d31, #0
    vcle.f32 d31, d31, #0
    vclt.f32 d31, d31, #0
