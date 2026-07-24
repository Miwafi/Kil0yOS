# Makefile for user programs

CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy

# User program runs in 64-bit mode without standard libraries
CFLAGS = -std=c11 -ffreestanding -O2 -fno-stack-protector -m64 -mno-red-zone \
         -fno-pie -fno-pic -mcmodel=small -mno-sse -mno-sse2 -mno-mmx \
         -I../include -D__USER__

LDFLAGS = -T user.ld -nostdlib -m elf_x86_64

SRCDIR = .
BUILDDIR = ../build/user

# User programs
PROGRAMS = hello.bin

.PHONY: all clean

all: $(PROGRAMS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

%.bin: $(BUILDDIR)/%.o
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) $< -o $@

clean:
	rm -f *.bin $(BUILDDIR)/*.o