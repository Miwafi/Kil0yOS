bits 16
org 0x8000

ap_trampoline_start:
    jmp short ap_code
    nop
    align 8

ap_pml4:  dd 0
          dd 0
ap_stack: dq 0
ap_entry: dq 0

ap_code:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Enable A20 via fast A20 gate
    in al, 0x92
    or al, 2
    out 0x92, al

    lgdt [gdtr]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x08:ap_pmode

align 4
gdtr:
    dw gdt_end - gdt - 1
    dd gdt

gdt:
    dq 0x0000000000000000  ; null
    dq 0x00CF9A000000FFFF  ; 32-bit code
    dq 0x00CF92000000FFFF  ; 32-bit data
    dq 0x00AF9A000000FFFF  ; 64-bit code (L=1, D=0)
    dq 0x00AF92000000FFFF  ; 64-bit data
gdt_end:

times 128 db 0

bits 32
ap_pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Enable PAE
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    ; Load PML4
    mov eax, [ap_pml4]
    mov cr3, eax

    ; Enable long mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Far jump to 64-bit code segment
    jmp 0x18:ap_lmode

times 128 db 0

bits 64
ap_lmode:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [ap_stack]
    mov rax, [ap_entry]
    jmp rax

ap_trampoline_end:
