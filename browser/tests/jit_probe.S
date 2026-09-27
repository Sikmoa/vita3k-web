/* M14 architectural probes. Never use r7 as a test/HLE ABI.
 * Reproduce jit_probe.inc with arm-vita-eabi-as -march=armv7-a, then
 * arm-vita-eabi-objcopy -O binary --only-section=.text.<name>.
 * All branches are local; no linking or relocations are needed.
 */
.syntax unified
.arch armv7-a
.arm
.section .text.arm_loop,"ax",%progbits
    mov r0, #0
    mov r1, #3
    mov r2, #7
1:  add r0, r0, r2
    sub r1, r1, #1
    cmp r1, #0
    bne 1b
    svc #0x42

.section .text.arm_conditional,"ax",%progbits
    mov r0, #7
    mov r1, #5
    add r2, r0, r1
    bne 1f
    mov r3, #99
1:  svc #0x42

.section .text.arm_add,"ax",%progbits
    adds r2, r0, r1
    svc #0x42
.section .text.arm_sub,"ax",%progbits
    subs r2, r0, r1
    svc #0x42
.section .text.arm_cmp,"ax",%progbits
    cmp r0, r1
    svc #0x42

.section .text.arm_patch,"ax",%progbits
    mov r0, #7
    svc #0x42

.section .text.arm_patch_11,"ax",%progbits
    mov r0, #11
.section .text.arm_patch_13,"ax",%progbits
    mov r0, #13
.section .text.arm_patch_17,"ax",%progbits
    mov r0, #17

.section .text.arm_unsupported,"ax",%progbits
    udf #0
.section .text.arm_memory_unsupported,"ax",%progbits
    ldr r2, [r0]
    svc #0x42
.section .text.arm_prefix_unsupported,"ax",%progbits
    mov r3, #99
    udf #0

.section .text.arm_budget,"ax",%progbits
1:  b 1b

/* Same layout used by interpreter_runtime_tests: NID at post-SVC PC+4.
 * Return slot must NOT execute: the host stops at the preimport callback.
 */
.section .text.arm_runtime,"ax",%progbits
    mov r0, #42
    svc #0x42
    mov pc, lr
    .word 0x12345678

.thumb
.section .text.thumb_loop,"ax",%progbits
    movs r0, #0
    movs r1, #3
1:  adds r0, #7
    subs r1, #1
    cmp r1, #0
    bne 1b
    svc #0x42

.section .text.thumb_add,"ax",%progbits
    adds r2, r0, r1
    svc #0x42
.section .text.thumb_sub,"ax",%progbits
    subs r2, r0, r1
    svc #0x42
.section .text.thumb_cmp,"ax",%progbits
    cmp r0, r1
    svc #0x42
.section .text.thumb_patch,"ax",%progbits
    movs r0, #7
    svc #0x42
.section .text.thumb_patch_11,"ax",%progbits
    movs r0, #11
.section .text.thumb_patch_13,"ax",%progbits
    movs r0, #13
.section .text.thumb_patch_17,"ax",%progbits
    movs r0, #17
.section .text.thumb_unsupported,"ax",%progbits
    udf #0
.section .text.thumb_memory_unsupported,"ax",%progbits
    ldr r2, [r0]
    svc #0x42
.section .text.thumb_prefix_unsupported,"ax",%progbits
    movs r3, #99
    udf #0
.section .text.thumb_budget,"ax",%progbits
1:  b 1b
