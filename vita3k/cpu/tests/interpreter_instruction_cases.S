// ARMv7-A encodings used by interpreter_instruction_tests.cpp.
// Verified with VitaSDK binutils (no hand-encoded instruction assumptions).
// Assemble: arm-vita-eabi-as -mcpu=cortex-a9 -mfpu=neon -o cases.o this-file.S
// Link: arm-vita-eabi-ld -Ttext=0x81000000 -e test_mov -o cases.elf cases.o
// The checked-in .inc is the .text bytes and global-symbol addresses of that ELF.
.syntax unified
.cpu cortex-a9
.fpu neon
.text
.thumb
.macro test name
.balign 4
.global \name
.thumb_func
\name:
.endm

test test_mov
    movw r3, #0xface
    movt r3, #0x8103
    mov.w r4, #0x08000000
    mov.w r5, #0xffffffff
    mov.w r6, #0x00ab00ab
    mov.w r8, #0xab00ab00
    mov.w r9, #0xabababab
    nop.w
    svc #0x73

test test_flags
    adds r0, r0, r1
    svc #1

test test_sub
    subs r0, r0, r1
    svc #1

test test_adc
    adcs r0, r1
    svc #1

test test_sbc
    sbcs r0, r1
    svc #1

test test_shift
    lsls r0, r1
    svc #1

test test_lsr
    lsrs r0, r1
    svc #1

test test_asr
    asrs r0, r1
    svc #1

test test_ror
    rors r0, r1
    svc #1

test test_imm_shift
    lsrs r0, r0, #32
    asrs r1, r1, #32
    svc #1

test test_it
    cmp r0, #0
    itte eq
    moveq r1, #0
    addeq r2, #1
    movwne r3, #0xdead
    it ne
    ldrne r4, [r5] // r5 deliberately unmapped on EQ path
    svc #0x25

test test_wide_flags
    adds.w r0, r0, r1, lsl #2
    svc #1

test test_memory
    str r1, [r0, #4]
    ldr r2, [r0, #4]
    strb r1, [r0, #8]
    ldrb r3, [r0, #8]
    strh r1, [r0, #10]
    ldrh r4, [r0, #10]
    movs r5, #8
    ldrsb r6, [r0, r5]
    movs r5, #10
    ldrsh r7, [r0, r5]
    str.w r1, [r0, #16]!
    ldr.w r8, [r0], #4
    str.w r2, [r0, r5, lsl #2]
    ldr.w r9, [r0, r5, lsl #2]
    strd r1, r2, [r0, #8]
    ldrd r10, r11, [r0, #8]
    strh.w r1, [r0, #40]
    ldrh.w r12, [r0, #40]
    svc #1

test test_stack
    push {r4-r7, lr}
    stmdb sp!, {r8-r11}
    movs r4, #0
    mov r8, r4
    ldmia sp!, {r8-r11}
    pop {r4-r7, pc}

test test_multi
    stmia r0!, {r1-r3}
    subs r0, #12
    ldmia r0!, {r4-r6}
    ldmia.n r7, {r0, r7} // no writeback if base in list, Thumb T1
    svc #1

test test_pc
    adr r0, 1f
    ldr r1, 1f
    mov r2, pc
    svc #1
.balign 4
1: .word 0x1234abcd

test test_branch
    movs r0, #0
    cbnz r0, 3f
    cbz r0, 1f
    b 3f
1:  bl 2f
    cmp r0, #42
    bne.w 3f
    beq.w 4f
3:  udf #1
2:  movs r0, #42
    bx lr
4:  svc #7

test test_interwork
    blx arm_target
    svc #9
.balign 4
.arm
arm_target:
    mov r0, #0x80000000
    mov pc, lr // actual load_self import return, ARMv7 ALUWritePC interworks

test test_vfp
    vpush {d8-d11}
    vldr d8, [r0, #8]
    vstr d8, [r0, #16]
    vdup.32 q8, r1
    vst1.32 {d16-d17}, [r0]!
    vpop {d8-d11}
    svc #1

test test_unsupported
    udf #42

test test_loop
    b test_loop

.balign 4
.global test_arm
.arm
test_arm:
    movs r0, #0x80000000
    moveq r1, #77
    movmi r2, #42
    svc #0x123456
    bx lr
    .word 0xfeedcafe // NID belongs PC+4 after SVC (past the BX)

test test_veneer
    bx pc
    nop
.arm
    ldr ip, [pc]
    add pc, ip, pc
    .word 1f - . - 4 + 1
.thumb
1:  movs r0, #42
    svc #11

test test_load_pc
    ldr.w pc, [r0]

test test_mov_pc
    mov pc, r0

test test_blx_reg
    blx r0
    svc #12

test test_it_svc
    cmp r0, #0
    it eq
    svceq #14
    svc #15
