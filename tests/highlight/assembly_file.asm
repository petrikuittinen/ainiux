; NASM-style x86-64 syntax fixture
global _start

section .data
message: db "Hello from assembly", 10
length: equ $ - message

section .text
_start:
    mov rax, 1          ; write syscall
    mov rdi, 1
    lea rsi, [rel message]
    mov rdx, length
    syscall

    mov rax, 60         ; exit syscall
    xor rdi, rdi
    syscall

