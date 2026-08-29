bits 64

global gdt_flush
gdt_flush:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

extern isr_handler

%macro ISR_NO_ERROR 1
    global isr%1
    isr%1:
        cli
        push qword 0
        push qword %1
        jmp isr_common_stub
%endmacro

%macro ISR_ERROR 1
    global isr%1
    isr%1:
        cli
        push qword %1
        jmp isr_common_stub
%endmacro

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR 8
ISR_NO_ERROR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_NO_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_NO_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_NO_ERROR 29
ISR_NO_ERROR 30
ISR_NO_ERROR 31

extern irq_handler

%macro IRQ 2
    global irq%1
    irq%1:
        cli
        push qword 0
        push qword %2
        jmp irq_common_stub
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; System call handler (int 0x80)
extern syscall_dispatcher
global syscall_entry
syscall_entry:
    cli
    ; Save user registers
    push rbp
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rdx
    push rcx
    push rbx
    push rax

    ; System call number in rax, args in rbx, rcx, rdx, r8, r9, r10
    ; Dispatcher signature: (num, a0, a1, a2, a3, a4, a5)
    ;   rdi=num, rsi=a0, rdx=a1, rcx=a2, r8=a3, r9=a4, [stack]=a5
    xchg rcx, rdx         ; rcx = orig rdx (a2), rdx = orig rcx (a1)
    mov rdi, rax          ; syscall number
    mov rsi, rbx          ; arg0
    push r10              ; arg5 (r8, r9 already hold a3, a4)
    call syscall_dispatcher
    add rsp, 8            ; pop arg5 slot (ret addr already popped by ret)
                          ; -> rsp at the saved-rax slot

    ; Store return value directly into the saved-rax slot on the stack
    mov [rsp], rax

    ; Restore user registers (in reverse order of push)
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    pop rbp

    ; Return to user mode
    iretq

; --- Linux-ABI syscall entry (the `syscall` instruction, LSTAR) -----
; Calling convention: number in RAX, args in RDI, RSI, RDX, R10, R8, R9.
; On entry the CPU has loaded CS from STAR[47:32] and SS from CS+8, put
; the user RIP in RCX and user RFLAGS in R11, and - because FMASK clears
; IF - interrupts are already off. RSP still points at the user stack,
; so we save it and switch to the process kernel stack first.
extern syscall_lnx_dispatch
extern syscall_kernel_rsp

global syscall_lnx_entry
syscall_lnx_entry:
    mov [lnx_user_rsp_tmp], rsp      ; remember the user stack pointer
    mov rsp, [syscall_kernel_rsp]    ; switch to the process kernel stack

    ; Hardware frame for the iretq back to ring 3
    push qword 0x23                  ; user SS (USER_DS | 3)
    push qword [lnx_user_rsp_tmp]    ; user RSP
    push r11                         ; user RFLAGS
    push qword 0x1B                  ; user CS (USER_CS | 3)
    push rcx                         ; user RIP

    ; Save GP registers (same order as irq_common_stub)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Saved-frame offsets: r15+0 r14+8 r13+16 r12+24 r11+32 r10+40
    ; r9+48 r8+56 rbp+64 rdi+72 rsi+80 rdx+88 rcx+96 rbx+104 rax+112
    mov rdi, [rsp + 112]             ; syscall number (rax)
    mov rsi, [rsp + 72]              ; a0 (rdi)
    mov rdx, [rsp + 80]              ; a1 (rsi)
    mov rcx, [rsp + 88]              ; a2 (rdx)
    mov r8,  [rsp + 40]              ; a3 (r10)
    mov r9,  [rsp + 48]              ; a4 (r8)
    mov rax, [rsp + 56]              ; a5 (r9)
    push rax                         ; 7th argument on the stack
    call syscall_lnx_dispatch
    add rsp, 8

    ; Store the return value into the saved-rax slot
    mov [rsp + 112], rax

    ; Restore user registers and return to ring 3
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    iretq

section .data
lnx_user_rsp_tmp: dq 0

section .text
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call isr_handler
    ; isr_handler normally returns the same frame pointer, but after a
    ; ring3 fault kill it returns the saved kernel-main frame rsp - the
    ; dying process's frame is abandoned (same mechanism as irq_common_stub).
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call irq_handler
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq
