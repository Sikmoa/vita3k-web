// Assembler-source ground truth for the M14 integer-loop benchmark.
.syntax unified
.arch armv7-a
.thumb
.text
.global jit_bench_start
.thumb_func
jit_bench_start:
    movs r0, #0
1:  adds r0, #1
    subs r1, #1
    bne 1b
    svc #0x42
