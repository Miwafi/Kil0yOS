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
pml5:
    resb 4096
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
; 256-gate flat IDT: installed right after the far jump so stray vectors
; between long-mode entry and the kernel's own interrupt init cannot hit
; the firmware IDT (whose gates reference the firmware CS 0x38 - absent
; from the kernel GDT -> #GP(0x38) -> #DF -> triple -> reset loop).
align 16
boot_idt:
    resb 256 * 16

section .data
mb_info_ptr:
    dd 0
entry_mode:
    dd 0                        ; call-push size at entry: 8=64-bit, 4=32/compat

section .text
global _start

; Poll-and-write one byte to COM1.  Identical semantics in 32-bit and
; 64-bit mode (IN/OUT address via DX, no absolute memory access) so
; boot-stage heartbeats can be dropped anywhere along the mode switch.
%macro serial_char 1
    mov dx, 0x3F8 + 5
%%poll:
    in al, dx
    test al, 0x20
    jz %%poll
    mov al, %1
    mov dx, 0x3F8
    out dx, al
%endmacro

; Fill the 256-gate boot IDT at [boot_idt]: vectors 0-31 -> boot_fault_<v>
; shields (print 'X' + vector letter, then halt - an exception in the boot
; window becomes VISIBLE on COM1 instead of triple-faulting through the
; firmware IDT), vectors 32-255 -> boot_stub (EOI + iretq stray-IRQ shield).
; Uses only eax/ebx/edi so the identical machine code assembles in 32-bit
; (compat) and 64-bit mode.  Requires OUR GDT to be the active one (gates
; carry selector 0x08).
%macro build_boot_idt 0
    mov edi, boot_idt
    xor ebx, ebx
%%fill:
    cmp ebx, 32
    jae %%irq
    mov eax, ebx
    shl eax, 7                 ; 128 bytes per boot_fault stub
    add eax, boot_fault_0
    jmp %%have
%%irq:
    mov eax, boot_stub
%%have:
    mov [edi], ax              ; offset 15:0
    shr eax, 16
    mov [edi+6], ax            ; offset 31:16
    mov word [edi+2], 0x08     ; kernel CS
    mov byte [edi+4], 0        ; IST = 0
    mov byte [edi+5], 0x8E     ; P | DPL0 | 64-bit interrupt gate
    mov dword [edi+8], 0       ; offset 63:32 (stubs < 4 GiB)
    mov dword [edi+12], 0      ; reserved
    inc ebx
    add edi, 16
    cmp ebx, 256
    jb %%fill
%endmacro

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
    cld                         ; DF=0: the rep stosd page-table clear below
                                ; must sweep FORWARD regardless of entry state

    ; --- Entry mode probe ------------------------------------------------
    ; "call rel32" pushes 8 bytes in true 64-bit mode but only 4 in 32-bit
    ; protected or compat mode, so the delta identifies which mode GRUB
    ; handed control over in.  Observed: BOTH OVMF and VMware EFI keep_bs
    ; enter TRUE 64-bit (probe 8; CS 0x38 / 0x18, LMA=1).  The value is
    ; kept as a diagnostic heartbeat and only labels the far-jump path -
    ; both paths now share the same direct EA jump (see .efi_paged).
    mov ebp, esp
    call .mode_probe
.mode_probe:
    mov eax, ebp                ; BEFORE - AFTER.  The call pushed 8 (64-bit)
    sub eax, esp                ; or 4 (compat) bytes BELOW the entry RSP, so
    mov edi, entry_mode         ; AFTER - BEFORE would wrap negative (the
    mov [edi], eax              ; original bug: 0xFFFFFFF8 still prints a '8'
                                ; low nibble but never compares equal to 8).

    ; Debug: print the raw probe delta (low nibble as 1 hex digit).  8 =
    ; true 64-bit entry, 4 = 32/compat entry.  Printed immediately at the
    ; store so a later clobber of the entry_mode cell becomes visible by
    ; comparing against the branch-point dump below.  Uses ECX, NOT EBX:
    ; EBX still carries GRUB's multiboot info pointer here (saved below).
    mov ecx, eax
    and cl, 0x0F
    cmp cl, 10
    jb .pbdig
    add cl, 'a' - 10
    jmp .pbrdy
.pbdig:
    add cl, '0'
.pbrdy:
    mov dx, 0x3F8 + 5
.pbpoll:
    in al, dx
    test al, 0x20
    jz .pbpoll
    mov al, cl                 ; digit -> AL after the poll (IN clobbers AL)
    mov dx, 0x3F8
    out dx, al

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

    ; Debug: dump CR4 to COM1 as 8 hex digits.  CR4.LA57 (bit 12) tells us
    ; whether the firmware runs 5-level paging (VMware UEFI does, OVMF does
    ; not) - the whole reason the kernel must install a matching PML5.
    mov esi, cr4
    mov ecx, 8
.cr4dump:
    rol esi, 4
    mov ebx, esi
    and ebx, 0x0F
    cmp bl, 10
    jb .cr4dig
    add bl, 'a' - 10
    jmp .cr4rdy
.cr4dig:
    add bl, '0'
.cr4rdy:
    mov dx, 0x3F8 + 5
.cr4wait:
    in al, dx
    test al, 0x20
    jz .cr4wait
    mov al, bl
    mov dx, 0x3F8
    out dx, al
    loop .cr4dump

    ; Debug: dump the entry CS selector and EFER.LMA.  This answers "which
    ; mode did GRUB hand control over in" directly on COM1: the raw CS
    ; selector plus the long-mode-active bit (LMA=1 with a 32-bit-looking
    ; CS means compat mode inside long mode; LMA=0 means plain protected).
    mov ax, cs
    shl eax, 16                ; CS lives in bits 15:0 - shift it to the top
    mov esi, eax               ; half so the 4-nibble rol dump prints the
    mov ecx, 4                 ; SELECTOR, not stale upper-EAX garbage
.cs4dump:
    rol esi, 4
    mov ebx, esi
    and bl, 0x0F               ; bl = digit value only
    cmp bl, 10
    jb .cs4dig
    add bl, 'a' - 10
    jmp .cs4rdy
.cs4dig:
    add bl, '0'
.cs4rdy:
    mov dx, 0x3F8 + 5
.cs4wait:
    in al, dx
    test al, 0x20
    jz .cs4wait
    mov al, bl                 ; digit -> AL only AFTER the poll (the IN
    mov dx, 0x3F8              ; above clobbers AL with the LSR value)
    out dx, al
    loop .cs4dump
    mov dx, 0x3F8 + 5
.lmawait:
    in al, dx
    test al, 0x20
    jz .lmawait
    mov ecx, 0xC0000080
    rdmsr
    and eax, 1 << 10           ; IA32_EFER.LMA
    shr eax, 10
    add al, '0'
    mov dx, 0x3F8
    out dx, al

    ; Set up stack before any C calls.
    mov esp, stack_top

    ; --- Clear page tables ---
    mov edi, pml5
    mov ecx, 7 * 1024        ; 7 pages = pml5..pd3 = 28672 bytes
    xor eax, eax
    rep stosd

    ; --- PML5[0] -> PML4 (only used when firmware runs 5-level paging) ---
    ; P+W+U: the walk ANDs U/S across every level, a supervisor-only PML5[0]
    ; would lock ring-3 out of the user half on LA57 firmware.
    mov eax, pml4
    or eax, 0x07             ; Present + Writable + User
    mov edi, pml5
    mov [edi], eax

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

    serial_char 'P'             ; heartbeat: page tables built

    ; --- Enable PAE + SSE for user mode ---
    ; OSFXSR (bit 9): without it every ring-3 SSE instruction raises #UD
    ; (musl __init_tls uses movq/punpcklqdq). OSXMMEXCPT (bit 10): route
    ; SIMD FP exceptions to #XM instead of #UD.
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; --- Load CR3 ---
    ; CR4.LA57 decides the top level: with 5-level paging active (VMware
    ; UEFI firmware) CR3 must name the PML5; with 4-level (BIOS/SeaBIOS,
    ; OVMF) it must name the PML4.  Loading the wrong one mis-decodes the
    ; hierarchy by one level - the "only A on serial then reset" boot loop
    ; on VMware, because the far-jump fetch faults under the firmware IDT.
    mov eax, cr4
    test eax, 1 << 12        ; CR4.LA57
    jz .cr3_4level
    mov eax, pml5
    mov cr3, eax
    jmp .cr3_done
.cr3_4level:
    mov eax, pml4
    mov cr3, eax
.cr3_done:
    serial_char 'C'             ; heartbeat: CR3 switched to kernel tables

    ; --- Enable long mode (IA32_EFER.LME) ---
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    serial_char 'E'             ; heartbeat: EFER.LME written

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

    ; --- Load 64-bit GDT, build IDT shield, far jump (32-bit encodings) ---
    lgdt [gdt64.pointer]
    serial_char 'g'            ; heartbeat: 32-bit (BIOS-style) branch taken
    build_boot_idt
    lidt [boot_idtr]           ; compat-mode lidt reads the 6-byte form
                               ; (limit + base low 32); boot_idtr serves
                               ; both the 6- and 10-byte reads
    serial_char 'I'            ; heartbeat: stub IDT live pre far-jump
    jmp 0x08:long_mode

.efi_paged:
bits 64
    ; CR0.PG already set: GRUB's EFI keep_bs entry.  Observed on BOTH OVMF
    ; and VMware: TRUE 64-bit mode (probe prints 8, CS=0x38/0x18, LMA=1).
    ; Every instruction below assembles with 32-bit forms (no REX) and
    ; executes identically in compat and 64-bit mode; the lgdt/lidt disp32
    ; forms are absolute in both.
    mov eax, gdt64.pointer64
    lgdt [eax]                 ; 6-byte form under compat, 10-byte in 64-bit:
                               ; boot_idtr's limit+low32 base is the same
    ; Shield BEFORE the far jump: from here on every exception lands on
    ; our own stubs (prints 'X' + vector letter + faulting RIP, then halts)
    ; instead of triple-faulting through the firmware IDT - the firmware
    ; CS 0x38 is absent from the active GDT once ours is loaded.
    build_boot_idt
    lidt [boot_idtr]
    serial_char 'I'            ; heartbeat: stub IDT live pre far-jump
    ; The entry_mode cell (printed at _start and re-read below) records
    ; which mode GRUB handed over in.  TRUE 64-bit entries (probe=8: both
    ; OVMF and VMware) reach long_mode through a far-RETURN trampoline:
    ; the indirect "jmp far qword [mem]" (67 48 FF /5) #GP(0)'d on VMware's
    ; vCPU with byte-identical inputs to OVMF (target canonical, selector
    ; valid, GDT ours - classic m16:32 misdecode signature, selector read
    ; as 0 from farptr+4), and the direct EA ptr16:32 form is
    ; architecturally #UD in 64-bit mode (confirmed: XG with RIP at the EA
    ; instruction).  RETFQ takes the target RIP + selector from OUR STACK
    ; - no farptr read, no far-jump opcode - and lands in the exact same
    ; state the far jump produced (CS=0x08, RIP=long_mode), which every
    ; downstream iretq (efi_gop stub shield) relies on.  COMPAT entries
    ; (probe=4) still use the EA far jump, which is valid there.
    mov edi, entry_mode
    ; Debug: print the entry_mode cell's low nibble as read HERE (compare
    ; against the probe-point digit: equal => cell intact, differ => it was
    ; clobbered between _start and the far jump).
    mov eax, [edi]
    mov ebx, eax
    and bl, 0x0F
    cmp bl, 10
    jb .bmdig
    add bl, 'a' - 10
    jmp .bmrdy
.bmdig:
    add bl, '0'
.bmrdy:
    mov dx, 0x3F8 + 5
.bmpoll:
    in al, dx
    test al, 0x20
    jz .bmpoll
    mov al, bl
    mov dx, 0x3F8
    out dx, al
    cmp dword [edi], 8
    je .jmp64
    serial_char 'e'            ; heartbeat: probe said compat (4)
bits 32
    jmp 0x08:long_mode         ; compat entry: EA far jump (valid here)
bits 64
.jmp64:
    serial_char 'm'            ; heartbeat: probe said true 64-bit (8)
    mov eax, long_mode         ; target RIP (long_mode < 4 GiB, zero-extends)
    push 0x08                  ; CS slot (8 bytes pushed in 64-bit mode)
    push rax                   ; RIP slot
    retfq                      ; pop RIP + CS -> long_mode with CS=0x08

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

    ; The boot IDT was built and lidt'ed BEFORE the far jump in both entry
    ; branches, so exceptions in the mode-switch window land on our own
    ; stubs instead of the firmware IDT.  'S' marks long-mode entry.
    serial_char 'S'             ; heartbeat: long mode reached, stub IDT live

    ; Pass multiboot info pointer to kernel_main (System V AMD64 ABI: RDI)
    mov edi, [mb_info_ptr]
    xor rax, rax
    mov eax, edi
    mov rdi, rax

    extern kernel_main
    serial_char 'J'             ; heartbeat: entering kernel_main
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

; Exception shield (targets of boot-IDT vectors 0-31): print 'X' + a vector
; letter + the FAULTING RIP as 8 hex digits, then halt forever.  An
; exception in the boot window becomes VISIBLE on COM1 and FREEZES the VM
; instead of triple-faulting through the firmware IDT into a silent reset
; loop.  Vector letter = vector + 'A': #DE=0->A #DB=1->B #UD=6->G #DF=8->I
; #TS=10->K #NP=11->L #SS=12->M #GP=13->N #PF=14->O #AC=17->R.  For vectors
; WITHOUT an error code the CPU pushes RIP at [rsp]; for the error-code
; vectors (#DF 8, #TS 10, #NP 11, #SS 12, #GP 13, #PF 14, #AC 17) the code
; sits at [rsp] and RIP at [rsp+8], so [rsp+8] is dumped there - the dump
; is ALWAYS the faulting instruction address.  Each stub is padded to
; exactly 128 bytes so the IDT fill loop can address them as
; boot_fault_0 + vector*128 (the unpadded body is ~80 bytes in bits 64).
%assign bf_v 0
%rep 32
boot_fault_ %+ bf_v:
    cli
    mov dx, 0x3F8 + 5
.wf_ %+ bf_v:
    in al, dx
    test al, 0x20
    jz .wf_ %+ bf_v
    mov al, 'X'
    mov dx, 0x3F8
    out dx, al
    mov dx, 0x3F8 + 5
.we_ %+ bf_v:
    in al, dx
    test al, 0x20
    jz .we_ %+ bf_v
    mov al, (bf_v + 0x41)
    mov dx, 0x3F8
    out dx, al
%if (bf_v = 8) || (bf_v = 10) || (bf_v = 11) || (bf_v = 12) || (bf_v = 13) || (bf_v = 14) || (bf_v = 17)
    mov ebx, [rsp+8]           ; error-code vector: [rsp]=code, [rsp+8]=RIP
%else
    mov ebx, [rsp]             ; no-error vector: [rsp]=RIP directly
%endif
    mov ecx, 8
.hx_ %+ bf_v:
    rol ebx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .hd_ %+ bf_v
    add al, 'a' - 10
    jmp .hp_ %+ bf_v
.hd_ %+ bf_v:
    add al, '0'
.hp_ %+ bf_v:
    mov ah, al                 ; stash digit in AH: the poll below clobbers
    mov dx, 0x3F8 + 5          ; AL with the LSR value
.hw_ %+ bf_v:
    in al, dx
    test al, 0x20
    jz .hw_ %+ bf_v
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    loop .hx_ %+ bf_v
.hf_ %+ bf_v:
    hlt
    jmp .hf_ %+ bf_v
    times (128 - ($ - boot_fault_ %+ bf_v)) nop
    %assign bf_v bf_v+1
%endrep

; Stray-IRQ shield (targets of boot-IDT vectors 32-255): EOI whatever
; raised the vector, then return.  x2APIC must be probed at runtime - an
; xAPIC MMIO EOI write (0xFEE000B0) raises #GP under x2APIC and the stub
; would re-enter forever (2.17.0 lesson).  The trailing 8259 EOIs are
; harmless while both PICs stay masked.
boot_stub:
    mov ecx, 0x1B               ; IA32_APIC_BASE
    rdmsr
    test eax, 1 << 10           ; EXT bit: x2APIC mode?
    jz .xapic
    mov ecx, 0x80B              ; x2APIC EOI register (MSR)
    xor eax, eax
    xor edx, edx
    wrmsr
    jmp .done
.xapic:
    xor eax, eax
    mov edx, 0xFEE000B0         ; LAPIC EOI (MMIO)
    mov [edx], eax
.done:
    mov al, 0x20
    out 0x20, al
    out 0xA0, al
    iretq

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
; Loader for the boot-window stub IDT (qword base: consumed by lidt in
; long mode, which reads the 10-byte descriptor form).
boot_idtr:
    dw (256 * 16 - 1)
    dq boot_idt
