<div align="center">
  <img src="assets/banner.svg" alt="kil0yOSnotCtOS" width="100%" />
  <h1>kil0yOS</h1>
  <p><strong>A 64-bit x86-64 microkernel operating system with a Linux compatibility layer</strong></p>

  <p>
    <a href="#">
      <img src="https://img.shields.io/github/stars/Miwafi/Kil0yOS?style=social" alt="GitHub stars" />
    </a>
    <a href="https://github.com/Miwafi/Kil0yOS/issues">
      <img src="https://img.shields.io/github/issues/Miwafi/Kil0yOS?style=social" alt="GitHub issues" />
    </a>
    <a href="https://github.com/Miwafi/Kil0yOS">
      <img src="https://img.shields.io/github/repo-size/Miwafi/Kil0yOS?style=social" alt="GitHub repo size" />
    </a>
  </p>

  <p>
    <a href="README.md"><strong>English</strong></a> |
    <a href="README.zh.md"><strong>简体中文</strong></a>
  </p>
</div>

## Features

### Core kernel
- **x86-64 long mode** with 4-level page tables and identity-mapped first 4 GiB
- **Physical Memory Manager (PMM)** — bitmap-based 4 KiB page frame allocator with Multiboot2 mmap parsing
- **Virtual Memory Manager (VMM)** — on-demand 4-level page table mapping, unmapping, and address translation, plus per-process address spaces (private CR3 roots, fork/exec lifecycle)
- **Kernel panic & assert** (`PANIC`, `ASSERT`) with serial + VGA output and CPU halt
- Heap allocation with integrity canaries and free-list verification
- 64-bit interrupt handling with PIC, ISRs, and IDT; GDT with proper long-mode descriptors
- Round-robin task scheduler with 64-bit context switching
- VGA text mode display and a **TempleOS-style tiling GUI desktop** (320x200 mode 13h)
- PS/2 keyboard and mouse input handling

### Linux compatibility (Linux-ABI)
- **Runs real Linux x86-64 ELF binaries** — static, dynamic PIE, and full ELF interpreter loading (`PT_INTERP`): busybox 1.36.1 (musl static, all ~390 applets), musl dynamic PIE, and glibc dynamic PIE programs all run
- **Process model**: `fork`/`vfork`/`clone`, `wait4`, `execve` with per-process address spaces; TTY line discipline with canonical-mode input
- **Linux syscall layer** (`syscall_lnx`): ~60 syscalls including `openat`, `statx`, `getdents64`, `mmap` (fd-backed), `brk`, `readv`/`writev`, `poll`, `futex`/`rseq` stubs, `arch_prctl(SET_FS)`, and the socket family
- **Linux VFS shim** (`lnxvfs`): fd tables, `/proc`-style basics, stat/dirent translation onto the internal filesystem
- **Unified multi-backend VFS**: persistent ext2 read-only root (`/`), FAT32 RAM disk, and an in-memory write overlay — `/bin` survives reboots
- **Debian package ecosystem**: `dpkg` frontend (status database, `-i/-l/-L/-r`, dependency checking with transaction mode) and `kilget` repo client (`sources.list`, RFC822 `Packages` index, SHA256 verification, topological dependency-ordered installs) — the real Ubuntu `libc6` installs and runs (verified by executing the installed `ld-linux-x86-64.so.2`)

### Network stack
- Intel E1000 and Realtek RTL8139 NIC drivers (PCI Vendor/Device ID matching)
- Ethernet / ARP / IPv4 / ICMP / UDP / **TCP** with sliding-window flow control, retransmission timers, and a 64 KiB receive ring
- DHCP auto-configuration with static fallback
- **TFTP client** (RFC 1350), **UDP DNS** resolution (`nslookup`), **HTTP/1.1 client** (busybox `wget` works)

### User programs
- **Ring 3 user programs** loaded from `/bin` with graphics + keyboard syscall interfaces
- Built-in **Pong game** (`exec /bin/pong.bin`) with AI opponent, flicker-free incremental rendering

## Prerequisites

- gcc (x86-64 cross-compilation support)
- nasm
- ld (GNU linker)
- grub-mkrescue
- qemu-system-x86_64

> **Note:** This is a 64-bit kernel. Ensure your toolchain supports `-m64` and your emulator/VM is configured for a 64-bit guest.
> For the Linux-ABI test programs (busybox, musl/glibc builds) a **WSL/Debian host toolchain** is used — see `tools/` scripts.

## Build

```bash
make
```

## Run

```bash
make run
```

## Commands

Built-in shell commands:

- ls - List directory contents
- cd - Change directory
- pwd - Print working directory
- mkdir - Create directory (supports path like `mkdir subdir/file`)
- rm - Remove file or directory
- touch - Create empty file
- cat - Display file contents
- edit - Edit file contents
- clear - Clear screen
- echo - Print text (supports redirect to file with >)
- whoami - Print current user
- date / time - Show current date / time
- version - Show OS version
- help - Show help information
- shutdown - Shut down the system (ACPI S5)
- net - Network info / subcommand (ping|ifconfig|netstat)
- ping - Send ICMP echo requests
- tftp - Download a file via TFTP (installs to /bin)
- dpkg - Package tool: `dpkg -i file.deb` | `-r pkg` | `-l` | `-L pkg`
- kilget - Repo client (apt-get equivalent): `kilget update|install|show|list|installed` (`apt-get` is an alias)
- exec - Run a user program from `/bin` (e.g. `exec /bin/hello.bin`, `exec /bin/pong.bin`)

Unknown commands are dispatched to **busybox** (`/bin/busybox <cmd>`), so the full applet set works: `find`, `grep`, `wget`, `nslookup`, `vi`, `ps`, `head`, `wc`, ...

### Packaging example

```text
$ echo deb http://10.0.2.2:8000 . > /etc/kilget/sources.list
$ kilget update
$ kilget install libc6
kilget: downloading libc6_2.35-0ubuntu3_amd64.deb ...
 [kilget] installed libc6
$ exec /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
...usage banner from the real glibc dynamic linker...
```

## GUI Desktop

Run the `gui` command to enter the graphical tiling desktop. Navigate the left menu with **arrow keys** and press **Enter** to switch panels.

### Interactive Shell

The **Shell** panel provides a fully interactive graphical shell supporting `ls`, `cd`, `mkdir`, `touch`, `pwd`, `shutdown`, and more.

![Shell GUI](assets/shellgui.png)

### CAT Viewer

Because every OS needs a cat.

![=^._.^=](assets/mew.png)

### System GUI

Wanna check the system status?

![System GUI](assets/systemgui.png)

### Pong Game

Run `exec /bin/pong.bin` for a game of Pong against the AI (first to 5 wins). Move with **W/S**, press **ESC** to return to the shell. Rendered through ring 3 graphics syscalls with incremental (flicker-free) updates.

## Project Structure

```
src/
  boot/               - Bootloader (Multiboot2 + long mode entry, Assembly)
  kernel/
    core/             - Kernel core (main, gdt, idt, isr, tss, smp)
      elf.c           - ELF loader (static + dynamic PIE, PT_INTERP interpreter loading)
      process.c       - Process model (fork/wait4/execve, per-process CR3)
      syscall.c       - Ring 3 syscall interface (graphics/keyboard)
      syscall_lnx.c   - Linux syscall table (Linux-ABI)
      lnxvfs.c        - Linux VFS shim (fd tables, statx, getdents64, ...)
      tty.c           - TTY line discipline
      uvm.c           - User virtual memory management
    drivers/          - Device drivers (disk, keyboard, mouse, pci, pit, power, rtc, vga, speaker)
    fs/               - Filesystems
      fs.c            - Multi-backend VFS (FAT RAM disk + ext2 root + MEM write overlay)
      ext2.c          - ext2 read-only driver
      edit.c          - Text editor
    lib/              - Kernel standard library (string.c, stdlib.c)
    mm/               - Memory management (memory.c: PMM/VMM/heap)
    net/              - Network stack
      netif.c         - Interface abstraction (NIC dispatch)
      ethernet.c      - Ethernet framing
      arp.c           - ARP
      ipv4.c          - IPv4
      icmp.c          - ICMP (ping)
      udp.c           - UDP
      tcp.c           - TCP (sliding window, retransmission, flow control)
      dhcp.c          - DHCP client
      tftp.c          - TFTP client (RFC 1350)
      http.c          - HTTP/1.1 GET client
      e1000.c         - Intel E1000 NIC driver
      rtl8139.c       - Realtek RTL8139 NIC driver
    pkg/              - Debian package ecosystem
      deb.c           - ar archive + .deb member extraction
      tar.c           - ustar unpacking
      inflate.c       - DEFLATE/gzip decompression
      sha256.c        - SHA-256
      dpkg.c          - dpkg frontend (status database, install/remove/list)
      kilget.c        - repo client (apt-get equivalent)
    sched/            - Task scheduler
    shell/            - Command-line shell (shell.c) + terminal (terminal.c)
    timer/            - Timer management (pit.c)

user/                 - Linux-ABI test programs (hello, nettest, probe_ld, hello_pthread)
tools/                - Build + acceptance tooling (busybox build, disk images, QEMU headless harnesses)
include/              - Header files
Makefile              - Build configuration
grub.cfg              - GRUB2 boot configuration
linker.ld             - 64-bit linker script
ROADMAP_LINUX_COMPAT.md - Linux compatibility roadmap (phases 0-4)
CHANGELOG.md          - Release notes
```

## License

GPL2.0
