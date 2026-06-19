CC = gcc
AS = nasm
LD = ld
QEMU = qemu-system-x86_64

CFLAGS = -std=c11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -m32
ASFLAGS = -f elf32
LDFLAGS = -T linker.ld -O2 -nostdlib -m elf_i386

SRCDIR = src
INCDIR = include
BUILDDIR = build

KERNEL_SRCS = $(SRCDIR)/kernel/main.c \
              $(SRCDIR)/kernel/gdt.c \
              $(SRCDIR)/kernel/idt.c \
              $(SRCDIR)/kernel/isr.c \
              $(SRCDIR)/kernel/interrupts.c \
              $(SRCDIR)/kernel/memory.c \
              $(SRCDIR)/kernel/vga.c \
              $(SRCDIR)/kernel/keyboard.c \
              $(SRCDIR)/kernel/string.c \
              $(SRCDIR)/kernel/stdlib.c \
              $(SRCDIR)/kernel/fs.c \
              $(SRCDIR)/kernel/shell.c

KERNEL_OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(KERNEL_SRCS))
KERNEL_ASM_OBJS = $(BUILDDIR)/kernel/isr_asm.o
BOOT_OBJ = $(BUILDDIR)/boot/boot.o

.PHONY: all clean run iso

all: iso

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

$(BOOT_OBJ): $(SRCDIR)/boot/boot.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ASM_OBJS): $(SRCDIR)/kernel/isr.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR)/kernel.bin: $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BOOT_OBJ)
	$(LD) $(LDFLAGS) $(BOOT_OBJ) $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) -o $@

$(BUILDDIR)/kil0yos.iso: $(BUILDDIR)/kernel.bin
	@mkdir -p $(BUILDDIR)/iso/boot/grub
	cp $(BUILDDIR)/kernel.bin $(BUILDDIR)/iso/boot/kil0yos.bin
	cp grub.cfg $(BUILDDIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILDDIR)/iso

iso: $(BUILDDIR)/kil0yos.iso

run: $(BUILDDIR)/kil0yos.iso
	$(QEMU) -cdrom $(BUILDDIR)/kil0yos.iso -m 512M

clean:
	rm -rf $(BUILDDIR)