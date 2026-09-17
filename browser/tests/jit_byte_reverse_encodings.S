// Assemble/disassemble with VitaSDK; this file is an encoding oracle for
// the production-JIT reversal fixtures.
.syntax unified
.arch armv7-a
.text
.arm
.global byte_reverse_arm
byte_reverse_arm:
    rev r0, r1
    rev16 r12, r14
    revsh r14, r12
    svc #0
.thumb
.thumb_func
.global byte_reverse_thumb
byte_reverse_thumb:
    rev r0, r1
    rev16 r7, r6
    revsh r6, r7
    svc #0
