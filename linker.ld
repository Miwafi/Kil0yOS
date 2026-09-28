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

    /* Linux driver initcalls (module_init compat). NOTE: the start/end
     * symbols MUST live INSIDE the output section - assigning them from
     * outside uses the location counter before ld applies the input
     * sections' alignment (2**4 here), leaving a hole that corrupts the
     * first table entry whenever .data ends at an unfavorable offset. */
    .compat_initcall : ALIGN(8) {
        __compat_initcall_start = .;
        KEEP(*(.compat_initcall))
        __compat_initcall_end = .;
    }

    .bss : {
        *(.bss)
    }
    kernel_end = .;
}
