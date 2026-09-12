.syntax unified
.thumb
.section .text.start, "ax", %progbits
.global _start
.type _start, %function
_start:
    movs r0, #42
    movs r7, #0
    svc #0
.size _start, . - _start
