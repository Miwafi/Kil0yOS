CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
QEMU = qemu-system-x86_64

CFLAGS = -std=c11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -m64 -mno-red-zone -fno-pie -mcmodel=large -mno-sse -mno-sse2 -mno-mmx -I$(INCDIR)
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -O2 -nostdlib -m elf_x86_64

SRCDIR = src
INCDIR = include
BUILDDIR = build

# --- Core (CPU architecture, entry, interrupts) ---
CORE_SRCS = $(SRCDIR)/kernel/core/main.c \
            $(SRCDIR)/kernel/core/gdt.c \
            $(SRCDIR)/kernel/core/idt.c \
            $(SRCDIR)/kernel/core/isr.c \
            $(SRCDIR)/kernel/core/interrupts.c \
            $(SRCDIR)/kernel/core/smp.c

# --- Memory Management ---
MM_SRCS = $(SRCDIR)/kernel/mm/memory.c

# --- Device Drivers ---
DRIVERS_SRCS = $(SRCDIR)/kernel/drivers/vga.c \
               $(SRCDIR)/kernel/drivers/keyboard.c \
               $(SRCDIR)/kernel/drivers/mouse.c \
               $(SRCDIR)/kernel/drivers/disk.c \
               $(SRCDIR)/kernel/drivers/device.c \
               $(SRCDIR)/kernel/drivers/power.c \
               $(SRCDIR)/kernel/drivers/pci.c \
               $(SRCDIR)/kernel/drivers/rtc.c \
               $(SRCDIR)/kernel/drivers/speaker.c

# --- Filesystem ---
FS_SRCS = $(SRCDIR)/kernel/fs/fs.c \
          $(SRCDIR)/kernel/fs/edit.c

# --- Standard Library ---
LIB_SRCS = $(SRCDIR)/kernel/lib/string.c \
           $(SRCDIR)/kernel/lib/stdlib.c

# --- Shell ---
SHELL_SRCS = $(SRCDIR)/kernel/shell/shell.c \
             $(SRCDIR)/kernel/shell/terminal.c

# --- Scheduler ---
SCHED_SRCS = $(SRCDIR)/kernel/sched/scheduler.c

# --- Timer ---
TIMER_SRCS = $(SRCDIR)/kernel/timer/pit.c

# --- All kernel sources ---
KERNEL_SRCS = $(CORE_SRCS) \
              $(MM_SRCS) \
              $(DRIVERS_SRCS) \
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
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ): $(SRCDIR)/boot/boot.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ASM_OBJS): $(SRCDIR)/kernel/core/isr.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR)/ap_trampoline.bin: $(SRCDIR)/kernel/core/ap_trampoline.asm | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $@

$(BUILDDIR)/kernel/core/ap_trampoline.o: $(BUILDDIR)/ap_trampoline.bin | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-x86-64 $< $@

$(BUILDDIR)/kernel.bin: $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BOOT_OBJ) $(BUILDDIR)/kernel/core/ap_trampoline.o
	$(LD) $(LDFLAGS) $(BOOT_OBJ) $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BUILDDIR)/kernel/core/ap_trampoline.o -o $@

$(BUILDDIR)/kil0yos.iso: $(BUILDDIR)/kernel.bin
	@mkdir -p $(BUILDDIR)/iso/boot/grub
	cp $(BUILDDIR)/kernel.bin $(BUILDDIR)/iso/boot/kil0yos.bin
	cp grub.cfg $(BUILDDIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILDDIR)/iso

iso: $(BUILDDIR)/kil0yos.iso

run: $(BUILDDIR)/kil0yos.iso
	$(QEMU) -cdrom $(BUILDDIR)/kil0yos.iso -m 512M -nographic -serial stdio

disk:
	dd if=/dev/zero of=disk.img bs=512 count=4096

clean:
	rm -rf $(BUILDDIR)
