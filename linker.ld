OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x100000;
    kernel_start = .;

    .text : {
        *(.multiboot2)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    /* Linux driver initcalls (module_init compat) */
    . = ALIGN(8);
    __compat_initcall_start = .;
    .compat_initcall : {
        KEEP(*(.compat_initcall))
    }
    __compat_initcall_end = .;

    .bss : {
        *(.bss)
    }
    kernel_end = .;
}
