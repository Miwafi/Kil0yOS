CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
QEMU = qemu-system-x86_64

CFLAGS = -std=c11 -ffreestanding -O2 -g -Wall -Wextra -fno-stack-protector -m64 -mno-red-zone -fno-pie -mcmodel=large -mno-sse -mno-sse2 -mno-mmx -I$(INCDIR) -MMD -MP
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
            $(SRCDIR)/kernel/core/smp.c \
            $(SRCDIR)/kernel/core/tss.c \
            $(SRCDIR)/kernel/core/process.c \
            $(SRCDIR)/kernel/core/uvm.c \
            $(SRCDIR)/kernel/core/elf.c \
            $(SRCDIR)/kernel/core/syscall.c \
            $(SRCDIR)/kernel/core/syscall_lnx.c \
            $(SRCDIR)/kernel/core/tty.c \
            $(SRCDIR)/kernel/core/lnxvfs.c

# --- Memory Management ---
MM_SRCS = $(SRCDIR)/kernel/mm/memory.c

# --- Device Drivers ---
DRIVERS_SRCS = $(SRCDIR)/kernel/drivers/vga.c \
               $(SRCDIR)/kernel/drivers/font_8x8.c \
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
          $(SRCDIR)/kernel/fs/ext2.c \
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

# --- Network ---
NET_SRCS = $(SRCDIR)/kernel/net/netif.c \
           $(SRCDIR)/kernel/net/ethernet.c \
           $(SRCDIR)/kernel/net/arp.c \
           $(SRCDIR)/kernel/net/ipv4.c \
           $(SRCDIR)/kernel/net/icmp.c \
           $(SRCDIR)/kernel/net/udp.c \
           $(SRCDIR)/kernel/net/dhcp.c \
           $(SRCDIR)/kernel/net/tftp.c \
           $(SRCDIR)/kernel/net/rtl8139.c \
           $(SRCDIR)/kernel/net/e1000.c

# --- All kernel sources ---
KERNEL_SRCS = $(CORE_SRCS) \
              $(MM_SRCS) \
              $(DRIVERS_SRCS) \
              $(FS_SRCS) \
              $(LIB_SRCS) \
              $(SHELL_SRCS) \
              $(SCHED_SRCS) \
              $(TIMER_SRCS) \
              $(NET_SRCS)

KERNEL_OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(KERNEL_SRCS))
KERNEL_ASM_OBJS = $(BUILDDIR)/kernel/core/isr_asm.o
BOOT_OBJ = $(BUILDDIR)/boot/boot.o

# --- User programs (embedded into the kernel image) ---
USER_CCFLAGS = -std=c11 -ffreestanding -O2 -fno-stack-protector -m64 -mno-red-zone \
               -fno-pie -fno-pic -mcmodel=small -mno-sse -mno-sse2 -mno-mmx \
               -I$(INCDIR) -D__USER__
USER_PROGRAMS = hello pong
USER_BINS = $(patsubst %, $(BUILDDIR)/user/%.bin, $(USER_PROGRAMS))
USER_BLOB_OBJS = $(patsubst %, $(BUILDDIR)/user_blob_%.o, $(USER_PROGRAMS))
.SECONDARY: $(USER_BINS) $(BUILDDIR)/user/hello.o $(BUILDDIR)/user/pong.o

.PHONY: all clean run iso

all: iso

# --- Linux-ABI ELF programs (musl static, Phase 0 acceptance test) ---
# Built only when musl-gcc is available (PATH or ~/musl/bin, WSL);
# embedded as /bin/hello-lnx. Must stay AFTER the 'all' target.
MUSL_GCC := $(shell command -v musl-gcc 2>/dev/null)
ifeq ($(MUSL_GCC),)
  ifneq ($(wildcard $(HOME)/musl/bin/musl-gcc),)
    MUSL_GCC := $(HOME)/musl/bin/musl-gcc
  endif
endif
LNX_USER_PROGRAMS := $(if $(MUSL_GCC),hello-lnx,)
LNX_BLOB_OBJS := $(patsubst %, $(BUILDDIR)/user_blob_%.o, $(LNX_USER_PROGRAMS))
.SECONDARY: $(BUILDDIR)/user/hello-lnx

$(BUILDDIR)/user/hello-lnx: user/elf/hello.c
	@mkdir -p $(dir $@)
	$(MUSL_GCC) -static -no-pie -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

$(BUILDDIR)/user_blob_hello-lnx.o: $(BUILDDIR)/user/hello-lnx
	printf 'section .rodata\nglobal user_hello_lnx_start\nuser_hello_lnx_start:\nincbin "%s"\nglobal user_hello_lnx_end\nuser_hello_lnx_end:\n' '$<' > $(BUILDDIR)/user_blob_hello-lnx.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_hello-lnx.s -o $@

# --- mini: freestanding Linux-ABI syscall probe (no libc needed) ---
.SECONDARY: $(BUILDDIR)/user/mini
MINI_BLOB_OBJ := $(BUILDDIR)/user_blob_mini.o

$(BUILDDIR)/user/mini: user/elf/mini.c
	@mkdir -p $(dir $@)
	$(CC) -static -no-pie -nostdlib -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

$(BUILDDIR)/user_blob_mini.o: $(BUILDDIR)/user/mini
	printf 'section .rodata\nglobal user_mini_start\nuser_mini_start:\nincbin "%s"\nglobal user_mini_end\nuser_mini_end:\n' '$<' > $(BUILDDIR)/user_blob_mini.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_mini.s -o $@

# --- mmt: freestanding brk/mmap/mprotect acceptance probe (no libc) ---
.SECONDARY: $(BUILDDIR)/user/mmt
MMT_BLOB_OBJ := $(BUILDDIR)/user_blob_mmt.o

$(BUILDDIR)/user/mmt: user/elf/mmt.c
	@mkdir -p $(dir $@)
	$(CC) -static -no-pie -nostdlib -fno-builtin -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

$(BUILDDIR)/user_blob_mmt.o: $(BUILDDIR)/user/mmt
	printf 'section .rodata\nglobal user_mmt_start\nuser_mmt_start:\nincbin "%s"\nglobal user_mmt_end\nuser_mmt_end:\n' '$<' > $(BUILDDIR)/user_blob_mmt.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_mmt.s -o $@

# --- busybox: musl static multi-call binary (Phase 1.3) -----------------
# Built in WSL at $(BUSYBOX_SRC) (see tools/build_busybox.sh). Embedded
# only when the binary exists, and installed to /bin/busybox at boot.
BUSYBOX_SRC := $(HOME)/busybox-1.36.1/busybox
BUSYBOX_BLOB := $(if $(wildcard $(BUSYBOX_SRC)),$(BUILDDIR)/user_blob_busybox.o,)

$(BUILDDIR)/user_blob_busybox.o: $(BUSYBOX_SRC)
	printf 'section .rodata\nglobal user_busybox_start\nuser_busybox_start:\nincbin "%s"\nglobal user_busybox_end\nuser_busybox_end:\n' '$<' > $(BUILDDIR)/user_blob_busybox.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_busybox.s -o $@

$(BUILDDIR)/user/hello.o: user/hello.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BUILDDIR)/user/pong.o: user/pong.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BUILDDIR)/user/%.bin: $(BUILDDIR)/user/%.o user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T user/user.ld -nostdlib -m elf_x86_64 $< -o $@

$(BUILDDIR)/user_blob_%.o: $(BUILDDIR)/user/%.bin
	printf 'section .rodata\nglobal user_$*_start\nuser_$*_start:\nincbin "%s"\nglobal user_$*_end\nuser_$*_end:\n' '$<' > $(BUILDDIR)/user_blob_$*.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_$*.s -o $@

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

$(BUILDDIR)/kernel.bin: $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BOOT_OBJ) $(BUILDDIR)/kernel/core/ap_trampoline.o $(USER_BLOB_OBJS) $(MINI_BLOB_OBJ) $(MMT_BLOB_OBJ) $(LNX_BLOB_OBJS) $(BUSYBOX_BLOB)
	$(LD) $(LDFLAGS) $(BOOT_OBJ) $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BUILDDIR)/kernel/core/ap_trampoline.o $(USER_BLOB_OBJS) $(MINI_BLOB_OBJ) $(MMT_BLOB_OBJ) $(LNX_BLOB_OBJS) $(BUSYBOX_BLOB) -o $@

$(BUILDDIR)/kil0yos.iso: $(BUILDDIR)/kernel.bin
	@mkdir -p $(BUILDDIR)/iso/boot/grub
	cp $(BUILDDIR)/kernel.bin $(BUILDDIR)/iso/boot/kil0yos.bin
	cp grub.cfg $(BUILDDIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILDDIR)/iso

iso: $(BUILDDIR)/kil0yos.iso

run: $(BUILDDIR)/kil0yos.iso
	$(QEMU) -cdrom $(BUILDDIR)/kil0yos.iso -m 512M -nographic -serial stdio -netdev user,id=net0 -device rtl8139,netdev=net0

disk:
	dd if=/dev/zero of=disk.img bs=512 count=4096

clean:
	rm -rf $(BUILDDIR)

# Header dependency tracking (generated by -MMD). Kept at the END of the
# file so the .d files' targets never become make's default goal.
-include $(KERNEL_OBJS:.o=.d)
