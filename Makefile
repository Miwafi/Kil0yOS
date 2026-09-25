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
            $(SRCDIR)/kernel/core/nmi_wdt.c \
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
               $(SRCDIR)/kernel/drivers/keyboard.c \
               $(SRCDIR)/kernel/drivers/mouse.c \
               $(SRCDIR)/kernel/drivers/disk.c \
               $(SRCDIR)/kernel/drivers/device.c \
               $(SRCDIR)/kernel/drivers/power.c \
               $(SRCDIR)/kernel/drivers/pci.c \
               $(SRCDIR)/kernel/drivers/rtc.c \
               $(SRCDIR)/kernel/drivers/speaker.c \
               $(SRCDIR)/kernel/drivers/efi_gop.c \
               $(SRCDIR)/kernel/drivers/fb.c \
               $(SRCDIR)/kernel/drivers/jpeg.c \
               $(SRCDIR)/kernel/drivers/audio.c \
               $(SRCDIR)/kernel/drivers/ac97.c \
               $(SRCDIR)/kernel/drivers/hda.c \
               $(SRCDIR)/kernel/drivers/mp3.c

# --- Filesystem ---
FS_SRCS = $(SRCDIR)/kernel/fs/fs.c \
          $(SRCDIR)/kernel/fs/ext2.c \
          $(SRCDIR)/kernel/fs/edit.c

# --- Package management (Phase 4: .deb / dpkg / kilget) ---
PKG_SRCS = $(SRCDIR)/kernel/pkg/inflate.c \
           $(SRCDIR)/kernel/pkg/tar.c \
           $(SRCDIR)/kernel/pkg/sha256.c \
           $(SRCDIR)/kernel/pkg/zstd.c \
           $(SRCDIR)/kernel/pkg/deb.c \
           $(SRCDIR)/kernel/pkg/dpkg.c \
           $(SRCDIR)/kernel/pkg/kilget.c

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
           $(SRCDIR)/kernel/net/dns.c \
           $(SRCDIR)/kernel/net/tcp.c \
           $(SRCDIR)/kernel/net/http.c \
           $(SRCDIR)/kernel/net/dhcp.c \
           $(SRCDIR)/kernel/net/tftp.c \
           $(SRCDIR)/kernel/net/rtl8139.c \
           $(SRCDIR)/kernel/net/rtl8111.c \
           $(SRCDIR)/kernel/net/e1000.c

# --- USB (UHCI + core + HID) ---
USB_SRCS = $(SRCDIR)/kernel/usb/usb.c \
           $(SRCDIR)/kernel/usb/uhci.c \
           $(SRCDIR)/kernel/usb/hid.c

# --- All kernel sources ---
KERNEL_SRCS = $(CORE_SRCS) \
              $(MM_SRCS) \
              $(DRIVERS_SRCS) \
              $(FS_SRCS) \
              $(PKG_SRCS) \
              $(LIB_SRCS) \
              $(SHELL_SRCS) \
              $(SCHED_SRCS) \
              $(TIMER_SRCS) \
              $(NET_SRCS) \
              $(USB_SRCS)

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

# --- Embedded blob staging (zstd) ----------------------------------------
# User-program blobs are staged through $(STAGE)/: when zstd exists on the
# build host the payload is stored zstd-compressed in the kernel image and
# decompressed at install time (kernel side: process.c blob_raw, detection
# by zstd frame magic - no separate format bookkeeping). Art assets
# (mp3/jpg) are already compressed formats and keep their raw verbatim
# rule below. $(MAKEFILE_LIST) as a stage prerequisite re-stages whenever
# the Makefile changes, so a zstd-availability flip cannot leave a stale
# mix of compressed and raw payloads behind.
ZSTD := $(shell command -v zstd 2>/dev/null)
STAGE := $(BUILDDIR)/blob_stage

# $(1) = blob/file name, $(2) = symbol base (user_<2>_start/_end),
# $(3) = source, $(4) = 1 compress via zstd when available.
# NOTE: $(if)/$(and) expansion functions, NOT ifeq/ifneq - conditionals
# inside a define are evaluated once at parse time, when $(1)..$(4) are
# still empty, which silently kept only the cp branch.
define BLOB_RULE
BLOB_STAGE_FILES += $(STAGE)/$(1).blob
$(STAGE)/$(1).blob: $(3) $(MAKEFILE_LIST)
	@mkdir -p $(STAGE)
	$(if $(and $(filter 1,$(4)),$(ZSTD)),zstd -19 -q -f $$< -o $$@,cp $$< $$@)

$(BUILDDIR)/user_blob_$(1).o: $(STAGE)/$(1).blob
	printf 'section .rodata\nglobal user_$(2)_start\nuser_$(2)_start:\nincbin "%s"\nglobal user_$(2)_end\nuser_$(2)_end:\n' '$$<' > $(BUILDDIR)/user_blob_$(1).s
	$$(AS) -f elf64 $(BUILDDIR)/user_blob_$(1).s -o $$@
endef

.PHONY: all clean run iso usb

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

# --- hello-dyn: musl DYNAMIC PIE + interpreter blob (Phase 3.0) ---------
# hello-dyn keeps default dynamic PIE linking; PT_INTERP points at
# /lib/ld-musl-x86_64.so.1 which is deployed from ~/musl/lib/libc.so
# (musl's ldso IS libc.so; its SONAME matches the DT_NEEDED "libc.so").
$(BUILDDIR)/user/hello-dyn: user/elf/hello.c
	@mkdir -p $(dir $@)
	$(MUSL_GCC) -O2 $< -o $@

LDSO_SRC := $(HOME)/musl/lib/libc.so
LDSO_BLOB := $(if $(wildcard $(LDSO_SRC)),$(BUILDDIR)/user_blob_ldmusl.o,)

SECONDARY_EXTRA := $(if $(MUSL_GCC),$(BUILDDIR)/user/hello-dyn $(BUILDDIR)/user/hello-lnx,)
.SECONDARY: $(SECONDARY_EXTRA)

# --- hello-glibc: glibc dynamic PIE + Debian rtld/libc (Phase 3.1) ------
# .interp=/lib64/ld-linux-x86-64.so.2; DT_NEEDED libc.so.6 is found via
# ld.so's built-in search path (/lib is in it) once deployed there.
GLIBC_LD_SRC := /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
GLIBC_LIBC_SRC := /lib/x86_64-linux-gnu/libc.so.6
GCC := $(if $(shell command -v gcc 2>/dev/null),gcc,)
GLIBC_PROG_BLOB := $(if $(GCC),$(BUILDDIR)/user_blob_hello-glibc.o,)
GLIBC_LD_BLOB := $(if $(wildcard $(GLIBC_LD_SRC)),$(BUILDDIR)/user_blob_ldlinux.o,)
GLIBC_LIBC_BLOB := $(if $(wildcard $(GLIBC_LIBC_SRC)),$(BUILDDIR)/user_blob_glibc.o,)

$(BUILDDIR)/user/hello-glibc: user/elf/hello.c
	@mkdir -p $(dir $@)
	$(GCC) -O2 $< -o $@

# --- hello-pthread: glibc dynamic + pthread primitives (Phase 3.2) ------
PTHREAD_BLOB := $(if $(GCC),$(BUILDDIR)/user_blob_pthread.o,)

$(BUILDDIR)/user/hello-pthread: user/elf/hello_pthread.c
	@mkdir -p $(dir $@)
	$(GCC) -O2 $< -o $@ -lpthread

# --- mini: freestanding Linux-ABI syscall probe (no libc needed) ---
.SECONDARY: $(BUILDDIR)/user/mini
MINI_BLOB_OBJ := $(BUILDDIR)/user_blob_mini.o

# --- probe-ld: replicates ld.so's libc.so.6 load sequence (debug) ---
.SECONDARY: $(BUILDDIR)/user/probe-ld
PROBE_BLOB_OBJ := $(if $(MUSL_GCC),$(BUILDDIR)/user_blob_probe-ld.o,)

$(BUILDDIR)/user/probe-ld: user/elf/probe_ld.c
	@mkdir -p $(dir $@)
	$(MUSL_GCC) -static -no-pie -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

$(BUILDDIR)/user/mini: user/elf/mini.c
	@mkdir -p $(dir $@)
	$(CC) -static -no-pie -nostdlib -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

# --- mmt: freestanding brk/mmap/mprotect acceptance probe (no libc) ---
.SECONDARY: $(BUILDDIR)/user/mmt
MMT_BLOB_OBJ := $(BUILDDIR)/user_blob_mmt.o

# --- nettest: freestanding TCP connect/GET probe (Phase 3.3, no libc) ---
.SECONDARY: $(BUILDDIR)/user/nettest
NETTEST_BLOB_OBJ := $(BUILDDIR)/user_blob_nettest.o

$(BUILDDIR)/user/nettest: user/elf/nettest.c
	@mkdir -p $(dir $@)
	$(CC) -static -no-pie -nostdlib -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

$(BUILDDIR)/user/mmt: user/elf/mmt.c
	@mkdir -p $(dir $@)
	$(CC) -static -no-pie -nostdlib -fno-builtin -O2 -Wl,-Ttext-segment=0x10000000 $< -o $@

# --- busybox: musl static multi-call binary (Phase 1.3) -----------------
# Built in WSL at $(BUSYBOX_SRC) (see tools/build_busybox.sh). Embedded
# only when the binary exists, and installed to /bin/busybox at boot.
BUSYBOX_SRC := $(HOME)/busybox-1.36.1/busybox
BUSYBOX_BLOB := $(if $(wildcard $(BUSYBOX_SRC)),$(BUILDDIR)/user_blob_busybox.o,)

# --- all staged blob rules (see "Embedded blob staging" above) ----------
$(eval $(call BLOB_RULE,hello,hello,$(BUILDDIR)/user/hello.bin,1))
$(eval $(call BLOB_RULE,pong,pong,$(BUILDDIR)/user/pong.bin,1))
$(eval $(call BLOB_RULE,hello-lnx,hello_lnx,$(BUILDDIR)/user/hello-lnx,1))
$(eval $(call BLOB_RULE,hello-dyn,hello_dyn,$(BUILDDIR)/user/hello-dyn,1))
$(eval $(call BLOB_RULE,ldmusl,ldmusl,$(LDSO_SRC),1))
$(eval $(call BLOB_RULE,hello-glibc,hello_glibc,$(BUILDDIR)/user/hello-glibc,1))
$(eval $(call BLOB_RULE,ldlinux,ldlinux,$(GLIBC_LD_SRC),1))
$(eval $(call BLOB_RULE,glibc,glibc,$(GLIBC_LIBC_SRC),1))
$(eval $(call BLOB_RULE,pthread,pthread,$(BUILDDIR)/user/hello-pthread,1))
$(eval $(call BLOB_RULE,probe-ld,probe_ld,$(BUILDDIR)/user/probe-ld,1))
$(eval $(call BLOB_RULE,mini,mini,$(BUILDDIR)/user/mini,1))
$(eval $(call BLOB_RULE,nettest,nettest,$(BUILDDIR)/user/nettest,1))
$(eval $(call BLOB_RULE,mmt,mmt,$(BUILDDIR)/user/mmt,1))
$(eval $(call BLOB_RULE,busybox,busybox,$(BUSYBOX_SRC),1))
.SECONDARY: $(BLOB_STAGE_FILES)

# Desktop art assets: every file in user/art/ is embedded verbatim and
# installed to /home/user/art at boot (process.c: user_install_blob).
# Symbol names sanitize '.', '-', '+' to '_' (user_art_cat_jpg etc.).
ART_FILES := $(wildcard user/art/*)
ART_BLOB_OBJS := $(patsubst user/art/%, $(BUILDDIR)/user_blob_art_%.o, $(ART_FILES))
art_sym = user_art_$(subst +,_,$(subst -,_,$(subst .,_,$(1))))

$(BUILDDIR)/user_blob_art_%.o: user/art/%
	printf 'section .rodata\nglobal $(call art_sym,$*)_start\n$(call art_sym,$*)_start:\nincbin "%s"\nglobal $(call art_sym,$*)_end\n$(call art_sym,$*)_end:\n' '$<' > $(BUILDDIR)/user_blob_art_$*.s
	$(AS) -f elf64 $(BUILDDIR)/user_blob_art_$*.s -o $@

$(BUILDDIR)/user/hello.o: user/hello.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BUILDDIR)/user/pong.o: user/pong.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BUILDDIR)/user/%.bin: $(BUILDDIR)/user/%.o user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T user/user.ld -nostdlib -m elf_x86_64 $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# minimp3 is float code, and the x86-64 psABI returns/passes float in XMM,
# so this one translation unit must be built with SSE even though the rest
# of the kernel is -mno-sse. mp3.c saves/restores the live FPU/SSE state and
# masks interrupts around every decode, so no other context ever sees it.
$(BUILDDIR)/kernel/drivers/mp3.o: $(SRCDIR)/kernel/drivers/mp3.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse -msse2 -mmmx -c $< -o $@

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

# Dynamic-PIE acceptance program (musl-gcc) + ldso blob (musl libc.so)
DYN_PROG_BLOB := $(if $(MUSL_GCC),$(BUILDDIR)/user_blob_hello-dyn.o,)

$(BUILDDIR)/kernel.bin: $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BOOT_OBJ) $(BUILDDIR)/kernel/core/ap_trampoline.o $(USER_BLOB_OBJS) $(MINI_BLOB_OBJ) $(MMT_BLOB_OBJ) $(NETTEST_BLOB_OBJ) $(LNX_BLOB_OBJS) $(DYN_PROG_BLOB) $(LDSO_BLOB) $(GLIBC_PROG_BLOB) $(GLIBC_LD_BLOB) $(GLIBC_LIBC_BLOB) $(PROBE_BLOB_OBJ) $(PTHREAD_BLOB) $(BUSYBOX_BLOB) $(ART_BLOB_OBJS)
	$(LD) $(LDFLAGS) $(BOOT_OBJ) $(KERNEL_OBJS) $(KERNEL_ASM_OBJS) $(BUILDDIR)/kernel/core/ap_trampoline.o $(USER_BLOB_OBJS) $(MINI_BLOB_OBJ) $(MMT_BLOB_OBJ) $(NETTEST_BLOB_OBJ) $(LNX_BLOB_OBJS) $(DYN_PROG_BLOB) $(LDSO_BLOB) $(GLIBC_PROG_BLOB) $(GLIBC_LD_BLOB) $(GLIBC_LIBC_BLOB) $(PROBE_BLOB_OBJ) $(PTHREAD_BLOB) $(BUSYBOX_BLOB) $(ART_BLOB_OBJS) -o $@

# GRUB 完整 Unicode 字体（含制表符字形），用于修复 gfxterm 菜单边框显示成 '?' 的问题。
# 使用构建机自带的 unicode.pf2；缺失时跳过（菜单退回纯文本，仍可正常引导）。
GRUB_FONT_SRC := $(firstword $(wildcard /usr/share/grub/unicode.pf2 /boot/grub/unicode.pf2))
GRUB_FONT_DST := $(if $(GRUB_FONT_SRC),$(BUILDDIR)/iso/boot/grub/fonts/unicode.pf2,)

$(BUILDDIR)/iso/boot/grub/fonts/unicode.pf2: $(GRUB_FONT_SRC)
	@mkdir -p $(dir $@)
	cp $(GRUB_FONT_SRC) $@

$(BUILDDIR)/kil0yos.iso: $(BUILDDIR)/kernel.bin $(GRUB_FONT_DST)
	@mkdir -p $(BUILDDIR)/iso/boot/grub
	cp $(BUILDDIR)/kernel.bin $(BUILDDIR)/iso/boot/kil0yos.bin
	cp grub.cfg $(BUILDDIR)/iso/boot/grub/grub.cfg
	# GRUB 主题壁纸（整屏黑底 + 左下角 Logo，随菜单一起打包进 ISO）
	cp assets/grub/background.png $(BUILDDIR)/iso/boot/grub/background.png
	# --mbr-force-bootable: grub-mkrescue's default hybrid MBR carries only
	# a GPT-protective entry (type 0xEE, no boot flag). Real BIOSes often
	# refuse to boot such a USB stick ("no bootable device"). xorriso adds
	# a pseudo boot-flag entry so picky BIOSes accept the stick.
	grub-mkrescue -o $@ $(BUILDDIR)/iso --mbr-force-bootable

iso: $(BUILDDIR)/kil0yos.iso

# Bare-metal USB stick image (MBR + FAT16 + GRUB): the hybrid ISO is
# ISO9660-over-USB which real BIOSes often fail to read, and GRUB needs
# insmod multiboot2 since command.lst is not shipped. See the script
# header for details. Run inside WSL.
usb: $(BUILDDIR)/kernel.bin
	bash tools/make_usb.sh $(BUILDDIR)/kil0yos-usb.img

run: $(BUILDDIR)/kil0yos.iso
	$(QEMU) -cdrom $(BUILDDIR)/kil0yos.iso -m 512M -display none -serial stdio -netdev user,id=net0 -device rtl8139,netdev=net0

disk:
	dd if=/dev/zero of=disk.img bs=512 count=4096

clean:
	rm -rf $(BUILDDIR)

# Header dependency tracking (generated by -MMD). Kept at the END of the
# file so the .d files' targets never become make's default goal.
-include $(KERNEL_OBJS:.o=.d)
