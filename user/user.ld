/* Linker script for user programs */

OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    /* User code starts at 256 MB - must match USER_CODE_BASE in
     * include/core/process.h (above the kernel heap arena) */
    . = 0x10000000;
    
    .text : {
        /* _start must be first: OUTPUT_FORMAT(binary) drops the ELF
         * header, so the kernel's raw-binary exec enters at offset 0. */
        *(.text.entry)
        *(.text)
        *(.text.*)
    }
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : {
        *(.data)
        *(.data.*)
    }
    
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}