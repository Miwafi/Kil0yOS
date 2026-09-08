bits 32

section .multiboot2
align 8
multiboot2_header:
    dd 0xE85250D6
    dd 0
    dd header_end - multiboot2_header
    dd -(0xE85250D6 + 0 + (header_end - multiboot2_header))
    ; Entry address tag (type 3)
    dw 3
    dw 0
    dd 16
    dd _start
    dd 0
    ; EFI boot services tag (type 7), flags=0:
    ; ask EFI builds of GRUB to keep boot services alive so the kernel can
    ; call GOP + GetMemoryMap + ExitBootServices itself.  BIOS builds ignore
    ; this tag entirely (verified in grub-2.12 multiboot_mbi2.c: the case
    ; body is compiled empty under GRUB_MACHINE_PCBIOS).
    dw 7
    dw 0
    dd 8
    ; End tag
    dw 0
    dw 0
    dd 8
header_end:

section .bss
align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pd0:
    resb 4096
pd1:
    resb 4096
pd2:
    resb 4096
pd3:
    resb 4096
stack_bottom:
    resb 32768
stack_top:

section .data
mb_info_ptr:
    dd 0

section .text
global _start
_start:
    ; GRUB2 sets:
    ;  EAX = 0x36D76289 (multiboot2 magic)
    ;  EBX = physical address of multiboot info structure
    ;
    ; GRUB may enter us in 32-bit protected mode (BIOS/SeaBIOS, paging off)
    ; OR already in 64-bit long mode (EFI, multiboot2 EFI_BS header tag:
    ; boot services must stay alive, so GRUB cannot leave long mode).
    ; Absolute-addressing encodings change meaning in long mode:
    ;  - A1/A3 moffs take a full 8-byte address (32-bit streams decode as
    ;    non-canonical -> #GP)
    ;  - ModRM disp32 becomes RIP-relative
    ; so this early code uses ONLY imm32-into-register + register-indirect
    ; memory access (identical semantics in both modes).
    cli                         ; no interrupts through GRUB's IDT on EFI

    ; Mask every 8259 IRQ line immediately.  On the EFI keep_bs path GRUB
    ; enters us with EFLAGS.IF=1 and the firmware IDT still live; worse,
    ; EDK2 re-asserts sti inside every boot-services call when it restores
    ; TPL, so IF cannot be kept off around our own code (serial polls,
    ; boot.asm page-table loops).  A PIT tick (vector 0x20, OVMF handler in
    ; pool memory) delivered through the firmware IDT under our GDT/CR3
    ; triple-faults - the drifting #UD/triple-fault during efi_gop_init.
    ; Masking both PICs blocks IRQ delivery regardless of IF; the kernel's
    ; own interrupt init remaps + unmasks later.  BIOS builds are unaffected
    ; (their PIC is remapped by interrupts.c the same way).
    mov dx, 0x21
    mov al, 0xFF
    out dx, al
    mov dx, 0xA1
    out dx, al

    mov edi, mb_info_ptr        ; absolute imm32, mode-agnostic
    mov [edi], ebx              ; save multiboot info pointer

    ; Debug: output 'A' to COM1
    mov dx, 0x3F8 + 5
.poll0:
    in al, dx
    test al, 0x20
    jz .poll0
    mov al, 'A'
    mov dx, 0x3F8
    out dx, al

    ; Set up stack before any C calls.
    mov esp, stack_top

    ; --- Clear page tables ---
    mov edi, pml4
    mov ecx, 6 * 1024        ; 6 pages = 24576 bytes = 6144 dwords
    xor eax, eax
    rep stosd

    ; --- PML4[0] -> PDPT ---
    mov eax, pdpt
    or eax, 0x03             ; Present + Writable
    mov edi, pml4
    mov [edi], eax

    ; --- PDPT[0..3] -> PD0..PD3 ---
    mov eax, pd0
    or eax, 0x03
    mov edi, pdpt
    mov [edi + 0 * 8], eax

    mov eax, pd1
    or eax, 0x03
    mov [edi + 1 * 8], eax

    mov eax, pd2
    or eax, 0x03
    mov [edi + 2 * 8], eax

    mov eax, pd3
    or eax, 0x03
    mov [edi + 3 * 8], eax

    ; --- PD0: identity map 0x00000000 - 0x3FFFFFFF (1 GB) ---
    mov edi, pd0
    mov eax, 0x00000083      ; Present + Writable + PS (2 MB page)
    mov ecx, 512
.pd0_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .pd0_loop

    ; --- PD1: identity map 0x40000000 - 0x7FFFFFFF (1 GB) ---
    mov edi, pd1
    mov eax, 0x40000083
    mov ecx, 512
.pd1_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .pd1_loop

    ; --- PD2: identity map 0x80000000 - 0xBFFFFFFF (1 GB) ---
    mov edi, pd2
    mov eax, 0x80000083
    mov ecx, 512
.pd2_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .pd2_loop

    ; --- PD3: identity map 0xC0000000 - 0xFFFFFFFF (1 GB) ---
    mov edi, pd3
    mov eax, 0xC0000083
    mov ecx, 512
.pd3_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .pd3_loop

    ; --- Enable PAE + SSE for user mode ---
    ; OSFXSR (bit 9): without it every ring-3 SSE instruction raises #UD
    ; (musl __init_tls uses movq/punpcklqdq). OSXMMEXCPT (bit 10): route
    ; SIMD FP exceptions to #XM instead of #UD.
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; --- Load CR3 with PML4 ---
    mov eax, pml4
    mov cr3, eax

    ; --- Enable long mode (IA32_EFER.LME) ---
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; --- Enable paging ---
    ; BIOS entry: paging off -> set PG and take the 32-bit EA far jump.
    ; EFI entry (GRUB keep_bs): long mode already active (PG+LMA set, GRUB
    ; CR3) -> our CR3 was loaded above, swap in our GDT and take an
    ; INDIRECT far jump (EA far jmp is invalid in long mode).
    mov eax, cr0
    test eax, 1 << 31
    jnz .efi_paged
    or eax, 1 << 31
    mov cr0, eax

    ; --- Load 64-bit GDT and far jump (32-bit encodings) ---
    lgdt [gdt64.pointer]
    jmp 0x08:long_mode

.efi_paged:
bits 64
    ; Only reachable with CR0.PG already set, i.e. GRUB's EFI keep_bs
    ; entry has us in long mode.  64-bit encodings are correct here.
    mov eax, gdt64.pointer64
    lgdt [eax]                 ; long-mode lgdt reads the 10-byte descriptor
    ; indirect far jump, m16:64 layout: qword RIP first, then word selector.
    ; REX.W (qword) is REQUIRED: without it FF /5 stays m16:32 and the CPU
    ; would read the selector from farptr+4 (the offset's high half!) -
    ; the exact #GP(0x10) seen on first boot.
    mov eax, efi_farptr
    jmp far qword [eax]

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Debug: output 'B' to COM1
    mov dx, 0x3F8 + 5
.poll1:
    in al, dx
    test al, 0x20
    jz .poll1
    mov al, 'B'
    mov dx, 0x3F8
    out dx, al

    ; Pass multiboot info pointer to kernel_main (System V AMD64 ABI: RDI)
    mov edi, [mb_info_ptr]
    xor rax, rax
    mov eax, edi
    mov rdi, rax

    extern kernel_main
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

section .rodata
align 8
gdt64:
    dq 0x0000000000000000      ; 0x00: Null
    dq 0x00209A0000000000      ; 0x08: 64-bit Kernel Code (DPL=0, L=1)
    dq 0x0000920000000000      ; 0x10: 64-bit Kernel Data (DPL=0)
    dq 0x0020FA0000000000      ; 0x18: 64-bit User Code   (DPL=3, L=1)
    dq 0x0000F20000000000      ; 0x20: 64-bit User Data   (DPL=3)
gdt64.pointer:
    dw (gdt64.pointer - gdt64 - 1)
    dd gdt64
align 8
gdt64.pointer64:
    dw (gdt64.pointer - gdt64 - 1)
    dq gdt64

section .data
align 8
; 64-bit far pointer (m16:64 layout): 64-bit RIP first, 16-bit selector
; AFTER it - the reverse of the 32-bit m16:32 layout.  Read by
; "o64 jmp far [eax]" with REX.W; a plain "jmp far [eax]" (m16:32) would
; take the selector from efi_farptr+4 and #GP.
efi_farptr:
    dq long_mode               ; 64-bit target RIP (long_mode < 4 GiB)
    dw 0x08                    ; our 64-bit kernel code selector
