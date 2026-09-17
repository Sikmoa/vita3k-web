// Unexecuted VitaSDK encoding oracle; compare with the production-JIT fixtures.
.syntax unified
.arch armv7-a
.fpu neon
.text
.arm
.global neon_compare_arm
neon_compare_arm:
    vcgt.s8 q15, q15, q1
    vcge.u16 d31, d2, d31
    vmax.s32 q15, q15, q1
    vmin.u8 d31, d31, d2
    vabd.s16 q15, q1, q15
    vaba.u32 d31, d31, d2
    vceq.i8 q15, q15, q1
    vtst.16 d31, d2, d31
    vcgt.s8 q15, q15, #0
    vcge.s16 d31, d31, #0
    vceq.i32 q15, q15, #0
    vcle.s8 d31, d31, #0
    vclt.s16 q15, q15, #0
    vabs.s32 d31, d31
    svc #0
.thumb
.thumb_func
.global neon_compare_thumb
neon_compare_thumb:
    vcgt.s8 q15, q15, q1
    vcge.u16 d31, d2, d31
    vmax.s32 q15, q15, q1
    vmin.u8 d31, d31, d2
    vabd.s16 q15, q1, q15
    vaba.u32 d31, d31, d2
    vceq.i8 q15, q15, q1
    vtst.16 d31, d2, d31
    vcgt.s8 q15, q15, #0
    vcge.s16 d31, d31, #0
    vceq.i32 q15, q15, #0
    vcle.s8 d31, d31, #0
    vclt.s16 q15, q15, #0
    vabs.s32 d31, d31
    svc #0
