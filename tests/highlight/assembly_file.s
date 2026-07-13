# GNU assembler AArch64 syntax fixture
.section .rodata
message:
    .asciz "Hello from AArch64"

.text
.global _start
.type _start, %function
_start:
    mov x0, #1
    ldr x1, =message
    mov x2, #18
    mov x8, #64
    svc #0

    mov x0, #0
    mov x8, #93
    svc #0

