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

# --- Core (CPU architecture, entry, interrupts) ---
CORE_SRCS = $(SRCDIR)/kernel/core/main.c \
            $(SRCDIR)/kernel/core/gdt.c \
            $(SRCDIR)/kernel/core/idt.c \
            $(SRCDIR)/kernel/core/isr.c \
            $(SRCDIR)/kernel/core/interrupts.c

# --- Memory Management ---
MM_SRCS = $(SRCDIR)/kernel/mm/memory.c

# --- Device Drivers ---
DRIVERS_SRCS = $(SRCDIR)/kernel/drivers/vga.c \
               $(SRCDIR)/kernel/drivers/keyboard.c \
               $(SRCDIR)/kernel/drivers/mouse.c \
               $(SRCDIR)/kernel/drivers/disk.c \
               $(SRCDIR)/kernel/drivers/device.c \
               $(SRCDIR)/kernel/drivers/power.c \
               $(SRCDIR)/kernel/drivers/pci.c

# --- Network ---
NET_SRCS = $(SRCDIR)/kernel/net/net.c \
           $(SRCDIR)/kernel/net/rtl8139.c \
           $(SRCDIR)/kernel/net/e1000.c \
           $(SRCDIR)/kernel/net/dhcp.c \
           $(SRCDIR)/kernel/net/udp.c

# --- Filesystem ---
FS_SRCS = $(SRCDIR)/kernel/fs/fs.c \
          $(SRCDIR)/kernel/fs/edit.c

# --- Standard Library ---
LIB_SRCS = $(SRCDIR)/kernel/lib/string.c \
           $(SRCDIR)/kernel/lib/stdlib.c

# --- Shell ---
SHELL_SRCS = $(SRCDIR)/kernel/shell/shell.c

# --- Scheduler ---
SCHED_SRCS = $(SRCDIR)/kernel/sched/scheduler.c

# --- Timer ---
TIMER_SRCS = $(SRCDIR)/kernel/timer/pit.c

# --- All kernel sources ---
KERNEL_SRCS = $(CORE_SRCS) \
              $(MM_SRCS) \
              $(DRIVERS_SRCS) \
              $(NET_SRCS) \
              $(FS_SRCS) \
              $(LIB_SRCS) \
              $(SHELL_SRCS) \
              $(SCHED_SRCS) \
              $(TIMER_SRCS)

KERNEL_OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(KERNEL_SRCS))
KERNEL_ASM_OBJS = $(BUILDDIR)/kernel/core/isr_asm.o
BOOT_OBJ = $(BUILDDIR)/boot/boot.o

.PHONY: all clean run iso

all: iso

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

$(BOOT_OBJ): $(SRCDIR)/boot/boot.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ASM_OBJS): $(SRCDIR)/kernel/core/isr.asm
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
	$(QEMU) -cdrom $(BUILDDIR)/kil0yos.iso -drive file=disk.img,format=raw -m 512M -nographic -serial stdio

disk:
	dd if=/dev/zero of=disk.img bs=512 count=4096

clean:
	rm -rf $(BUILDDIR)
