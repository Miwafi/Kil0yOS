# Kil0yOS

A 32-bit microkernel operating system written in C and Assembly.

## Features

- Memory management with heap allocation
- VGA text mode display
- PS/2 keyboard input handling
- Interrupt handling with PIC and ISRs
- GDT and IDT setup
- In-memory filesystem with directories and files
- Command-line shell with built-in commands
- File read/write operations

## Prerequisites

- gcc (32-bit cross-compilation support)
- nasm
- ld (GNU linker)
- grub-mkrescue
- qemu-system-x86_64

## Build

```bash
make
```

## Run

```bash
make run
```

## Commands

- ls - List directory contents
- cd - Change directory
- pwd - Print working directory
- mkdir - Create directory
- rm - Remove file or directory
- touch - Create empty file
- cat - Display file contents
- clear - Clear screen
- echo - Print text (supports redirect to file with >)
- whoami - Print current user
- version - Show OS version
- help - Show help information
- shutdown - Shut down the system

## Project Structure

```
src/
  boot/          - Bootloader (Assembly)
  kernel/        - Kernel source code (C)
    main.c       - Kernel entry point
    gdt.c        - Global Descriptor Table
    idt.c        - Interrupt Descriptor Table
    isr.c        - Interrupt Service Routines
    interrupts.c - PIC initialization
    memory.c     - Heap memory management
    vga.c        - VGA display driver
    keyboard.c   - Keyboard driver
    string.c     - String utilities
    stdlib.c     - Standard library functions
    fs.c         - Filesystem implementation
    shell.c      - Command shell

include/         - Header files
Makefile         - Build configuration
linker.ld        - Linker script
grub.cfg         - GRUB configuration
```

## License

Apache 2.0 License
