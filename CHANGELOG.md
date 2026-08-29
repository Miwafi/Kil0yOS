# Changelog
 All notable changes to this project will be documented in this file.
 The format follows Keep a Changelog and this project adheres to Semantic Versioning.

## [2.10.0] - 2026-08-30
This release completes **Phase 0 of the Linux compatibility roadmap** (M0 milestone): Kil0yOS can now load and run static Linux x86-64 ELF binaries — a `musl-gcc -static` hello world runs end-to-end, performing the full musl runtime initialization (`arch_prctl(SET_FS)`, `set_tid_address`, `mmap`, `brk`, ...) and exiting cleanly via `exit_group` back to the shell. This required an ELF64 loader, a per-process user VM manager, a native `syscall`-instruction ABI with a Linux-compatible handler table, and three deep CPU/virtual-memory fixes (EFER.NXE, huge-page split attribute, CR4.OSFXSR). Also includes the previously uncommitted network/timer hardening from the ping-debugging sessions: unified PIT clock, e1000/E1000E flash-NVM support, VID:DID-only NIC matching, and 1-second ping pacing.

### Added
- **ELF64 loader** (`elf.c` / `elf.h`): parses ELF header + program headers, maps `PT_LOAD` segments into the user address space, zeroes `p_memsz > p_filesz` (BSS), honors `PT_TLS` layout, and returns the entry point plus `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ` auxv data. The legacy KIL0-header and raw-binary paths remain and are auto-detected by magic.
- **User VM manager** (`uvm.c` / `uvm.h`): per-process region bookkeeping (`vm_region_t` in the PCB) over the shared kernel page tables. Provides `uvm_map_range` (allocates fresh frames, NX by default for non-exec pages, merges overlapping USER mappings), `uvm_unmap_range`, `uvm_change_prot`, an anonymous `mmap` bump arena (`UVM_MMAP_BASE`), and the `brk` heap anchored at the page-aligned end of the ELF image.
- **Linux-ABI syscall layer** (`syscall_lnx.c` / `syscall_lnx.h`): native `syscall`-instruction entry (MSR `EFER.SCE`, `STAR`, `LSTAR`, `FMASK`; kernel-stack switch via `syscall_kernel_rsp` because `syscall` does not use the TSS) plus a dispatch table implementing `write`/`writev` (VGA + COM1 console), `read`, `mmap`/`munmap`/`mprotect`, `brk`, `getpid`/`gettid`, `uname`, `getuid`/`getgid`/`geteuid`/`getegid`, `clock_gettime`, `arch_prctl(ARCH_SET_FS)` (FS_BASE MSR), `set_tid_address`, `getrandom`, `ioctl` (TTY defaults), `exit`/`exit_group`, and stubs for `rt_sigaction`/`rt_sigprocmask`/`set_robust_list`/`futex` — enough for musl's full single-threaded runtime init with no `ENOSYS`.
- **Unified exec path (Phase 0.6)** (`exec_load_program`): one code path for ELF64 / KIL0-header / raw binaries; lays out `argc/argv/envp/auxv` on the user stack Linux-style and supports `exec <prog> [args...]` from the shell. The old inline exec logic in `shell.c` (~120 lines) was replaced by a call into it.
- **Test probes embedded in the image**: `/bin/mini` (freestanding write/exit_group probe) and `/bin/mmt` (brk 1 MiB grow/roundtrip/shrink + 64 KiB anonymous mmap + mprotect RO↔RW + munmap — the Phase 0.2/0.3/0.4 acceptance checks). Both built with `-nostdlib -fno-builtin` and `.incbin`-embedded; `/bin/hello-lnx` (musl static) is added when `musl-gcc` is available.
- **`tools/lnx_qemu_test.py`**: headless QEMU regression — boots the ISO, types `exec /bin/<prog>` sequences via the monitor, and prints serial + qemu `-d int` logs (used to verify mini/mmt/hello-lnx with zero exceptions and three clean exits). Plus `tools/elf_qemu_test.py` and the ping diagnostic helpers (`ping_qemu_test.py`, `run_ping_diag*.sh`) from the network sessions.
- **`ROADMAP_LINUX_COMPAT.md`**: the full Linux-compatibility plan (Phase 0–4, from ELF loader to `apt install`); Phase 0 is now complete.
- **e1000/E1000E (82574L/82574LA) support**: device IDs `0x10D3`/`0x10F6` accepted; flash-backed NVM parts get their MAC word-by-word through `EERD` when RAL/RAH are empty, an `EE_RST` kick after global reset, and the driver waits for `EECD.AUTO_RD` (warn-and-continue on VMs that never set it).
- **PCI tree boot log**: one line per device (`[pci] 000:03.0 8086:100e 0200`) so the whole PCI topology is visible on any machine.

### Fixed
- **User pages mapped to nonexistent physical memory (CRITICAL, killed ELF exec)**: `vmm_map_page()` split 2 MB identity huge pages into 4 KB entries marked `VMM_USER`, so `uvm_map_range()` mistook the identity aliases for legitimate user mappings and reused their physical addresses — the user stack at VA `0x7fffe000` aliased PA `0x7fffe000`, which does not exist in a 512 MB guest (writes vanished, auxv read as zeros, musl returned into RIP=0). Identity-alias entries are now supervisor-only, so user mappings always get fresh frames.
- **Reserved-bit #PF on every non-exec user page (CRITICAL)**: `uvm` sets the NX bit (PTE bit 63) on non-executable user pages, but `EFER.NXE` was never enabled — with NXE=0 the CPU treats that bit as reserved and raises #PF err=0xa on first access. `vmm_init()` now sets `EFER.NXE` and reloads CR3.
- **Ring-3 SSE raised #UD (CRITICAL for musl)**: `__init_tls` uses SSE (`movq rax,xmm0` / `punpcklqdq`), but `CR4.OSFXSR`/`OSXMMEXCPT` were never set, so the first SSE instruction in user mode faulted with #UD. Both boot.asm and the AP trampoline now enable bits 9+10 of CR4.
- **Kernel clock ran slow / dual-clock inconsistency**: `pit_uptime_us()` polled the PIT countdown register with wrap-detection that under-counted (10x slow clock), and interrupt context saw the polling clock while mainline saw the tick clock — ARP entries appeared to expire instantly. After boot, one clock source is used everywhere: the IRQ0 tick clock, anchored once to the polling clock at `kernel_main` so uptime never jumps backwards; sub-tick resolution comes from the countdown register.
- **`ping` fired requests as fast as the NIC allowed**: replies are now awaited against a `pit_uptime_us()` deadline with NIC polling, and each request is paced to a 1-second period like real ping.
- **VMware e1000 (82545EM) rejected by class filter**: `netif_probe()` matched NICs by PCI class (`0200`), but VMware's e1000 reports subclass `0x10`. Matching is now VID:DID-only (RTL8139 0x10EC:0x8139; e1000 0x8086:0x100E/0x100F/0x10D3/0x10F6), and unsupported network-class devices are logged with their identity (e.g. vmxnet3) instead of being skipped silently.
- **NIC IRQ handlers registered for invalid lines**: `register_irq_handler`/`pic_enable_irq` guarded to `< 16`; unknown PCI interrupt lines (0/0xFF) fall back to pure RX polling via `g_netif.poll`.
- **e1000 diagnostics**: failed BAR composition and reset timeouts now log a reason instead of silently returning -1.

### Changed
- Version strings bumped to 2.10.0 (boot banner, `version` command, GUI title bar).
- `pcap_dump.py` decodes ARP/ICMP/UDP flows with relative timestamps.
- `.gitignore` adds `LinuxSample/`.
- Scheduler samples the user-mode RIP/RSP once every 64 ticks to serial (temporary bring-up aid).

### File Changes
- `include/core/elf.h`, `src/kernel/core/elf.c`: ELF64 loader (new)
- `include/core/uvm.h`, `src/kernel/core/uvm.c`: user VM manager (new)
- `include/core/syscall_lnx.h`, `src/kernel/core/syscall_lnx.c`: Linux-ABI syscall layer (new)
- `src/kernel/core/isr.asm`: `syscall_lnx_entry` — native `syscall` entry stub
- `src/kernel/core/process.c`, `include/core/process.h`: PCB VM regions, unified `exec_load_program`, argv/envp/auxv layout, `/bin/mini` + `/bin/mmt` install
- `src/kernel/core/syscall.c`: hook `syscall_lnx_init()`
- `src/kernel/mm/memory.c`, `include/mm/memory.h`: EFER.NXE, supervisor-only huge-page split, `vmm_get_pte()`
- `src/boot/boot.asm`, `src/kernel/core/ap_trampoline.asm`: CR4 OSFXSR/OSXMMEXCPT
- `src/kernel/timer/pit.c`, `src/kernel/core/main.c`: unified uptime clock
- `src/kernel/net/icmp.c`: ping deadline + 1 s pacing
- `src/kernel/net/e1000.c`, `include/net/e1000.h`: E1000E support, EERD MAC, diagnostics
- `src/kernel/net/netif.c`, `src/kernel/net/rtl8139.c`, `src/kernel/core/interrupts.c`: VID:DID matching, IRQ guards, polling fallback
- `src/kernel/drivers/pci.c`: PCI tree boot log
- `src/kernel/shell/shell.c`: `exec` via unified path with args, version bump
- `src/kernel/sched/scheduler.c`: user RIP sampling (temporary)
- `user/elf/mini.c`, `user/elf/mmt.c`, `user/elf/hello.c`: test probes + musl hello (new)
- `Makefile`: ELF probe builds (musl + freestanding), blob embedding
- `tools/lnx_qemu_test.py`, `tools/elf_qemu_test.py`, `tools/ping_qemu_test.py`, `tools/run_ping_diag.sh`, `tools/run_ping_diag2.sh`, `tools/pcap_dump.py`: test/diagnostic tooling
- `ROADMAP_LINUX_COMPAT.md`: Linux compatibility roadmap (new)
- `CHANGELOG.md`: this entry

### Notes
- Regression (`tools/lnx_qemu_test.py /bin/mini /bin/mmt /bin/hello-lnx`, QEMU headless): `MINI OK`, `MM OK`, `hello from musl static ELF on Kil0yOS!` + `argc=1 argv0=/bin/hello-lnx` + `write(2) works`, **zero CPU exceptions**, three clean `exit_group`s with the shell recovered after each — the Phase 0 M0 milestone.

## [2.9.0] - 2026-08-29
This release adds the first Ring 3 game: a built-in Pong with an AI opponent, backed by new graphics/keyboard system calls and an automated headless-QEMU regression harness. Along the way it fixes a boot crash under QEMU's default e1000 NIC (BAR composition), and a user-program entry-point mislink that made any raw binary whose `_start` was not the first linked function jump to garbage.

### Added
- **Pong game** (`user/pong.c` → `/bin/pong.bin`): ring 3 game with AI opponent, W/S paddle control, 5-point match, banner/winner screens, ESC returns to the shell. Renders through the new graphics syscalls with **incremental (flicker-free) updates** — each frame erases only the ball's old square and moved paddles, and redraws the dashed center line / score only when actually covered or changed; the full scene is drawn once per match via `draw_static()`. Installed into the filesystem at boot next to `hello.bin`.
- **Graphics & keyboard syscalls** (`SYS_GFX_MODE` / `SYS_GFX_CLEAR` / `SYS_GFX_RECT` / `SYS_GFX_TEXT` / `SYS_KEY_POLL`): ring 3 programs never touch the 0xA0000 framebuffer directly (identity map is kernel-only) — mode 13h switching, rectangle fill, string drawing, and non-blocking key polling are exposed through the syscall interface. `keyboard_getc()` values feed `SYS_KEY_POLL`; the VGA drawing primitives are no-ops while the display is in text mode.
- **Multi-user-program build**: `Makefile` now builds `USER_PROGRAMS = hello pong`, links each raw binary with `user/user.ld`, embeds them as `.incbin` blobs (`user_blob_*.o`), and `user_programs_install()` writes both into `/bin` at boot.
- **`tools/pong_qemu_test.py`**: headless QEMU regression that boots the ISO, types `exec /bin/pong.bin` into the shell via the monitor, sends W/S/ESC keystrokes, and screendumps/serial-checks each stage (text-mode shell → mode 13h banner → game field → ESC restores text mode, no kernel exceptions).

### Fixed
- **Boot #GP with QEMU's default e1000 NIC (CRITICAL)**: `e1000_init()` unconditionally composed the MMIO base as `bar0 | (bar1 << 32)`, but the 82540EM (0x100E, QEMU default) has a *32-bit memory BAR0 plus an I/O-port BAR1* (0x0000c001) — the composition produced the non-canonical address `0xc001feb80000` and the first MMIO read raised #GP (err=0, kernel mode) at `e1000_init+0x275`, dead-booting before the shell. The BAR is now composed only when BAR0's type bits declare a 64-bit memory BAR; I/O-space BARs and all-ones reads are rejected. VMware's 82545EM (0x100F, a real 64-bit BAR) is unaffected but now handled correctly too.
- **Raw-binary user programs entered at the wrong address (CRITICAL for pong)**: `OUTPUT_FORMAT(binary)` in `user/user.ld` drops the ELF header, so the kernel's raw exec enters at the image base — but the linker had placed `fmt_int.part.0` at offset 0 and `_start` at 0x430, so pong executed garbage that jumped to RIP=0 (#PF err=5, user mode) and then cascaded into a kernel #GP inside `irq_common_stub`'s IRET path. `user/user.ld` now links a `.text.entry` section first, and both user programs' `_start` carry `__attribute__((section(".text.entry")))` (`hello.bin` only worked by luck of link order).
- **Pong never entered mode 13h**: `_start` only called `SYS_GFX_MODE(0)` on exit; every drawing syscall silently no-oped in text mode so the game appeared to run "invisible". `_start` now enters mode 13h before drawing the banner.

### Changed
- Version strings bumped to 2.9.0 (boot banner, `version` command, GUI title bar).
- README (EN/zh): documented Ring 3 user programs, the Pong game, and the previously missing `exec` command.

### File Changes
- `user/pong.c`: Pong game (new)
- `user/hello.c`: `_start` moved to `.text.entry`
- `user/user.ld`: `.text.entry` ordered first
- `Makefile`: multi-user-program build + blob embedding
- `src/kernel/core/process.c`: pong blob install into `/bin`
- `src/kernel/core/syscall.c`: GFX/KEY syscall handlers
- `include/core/syscall.h`: syscall numbers 10-14
- `src/kernel/net/e1000.c`: type-safe BAR composition
- `src/kernel/core/main.c`, `src/kernel/shell/shell.c`: version bump
- `tools/pong_qemu_test.py`: headless regression harness (new)
- `README.md`, `README.zh.md`, `CHANGELOG.md`: documentation

### Notes
- Regression-verified with `tools/pong_qemu_test.py` on the default QEMU e1000 NIC: DHCP completes, shell boots, `exec /bin/pong.bin` enters mode 13h (screendump 640x400 doubled), W/S move the paddles, ESC restores text mode for the shell, and the serial log shows a clean `[proc] exit syscall` with no exceptions.

## [2.8.0] - 2026-08-29
This release makes network configuration self-adaptive: the kernel now runs a DHCP client at boot (DISCOVER → OFFER → REQUEST → ACK) instead of hardcoding the QEMU slirp static address, with a static fallback when no server answers. Along the way it fixes three RTL8139 driver bugs that made the NIC transmit all-zero frames and never receive, adds VMware's e1000 (82545EM) device ID, and hardens every port-I/O accessor with compiler ordering barriers.

### Added
- **DHCP auto-configuration client** (`dhcp.c` / `dhcp.h`): full DISCOVER → OFFER → REQUEST → ACK handshake over a UDP socket (port 68), transaction ID derived from the NIC MAC, timeouts driven by the validated polling clock `pit_uptime_us()` (reliable with interrupts disabled during boot). On success it fills `iface->ip` / `netmask` / `gateway`; on failure the caller falls back to the classic static QEMU user-network settings (10.0.2.15/24, gw 10.0.2.2) and logs `DHCP failed, using static fallback`. Works with both QEMU slirp (10.0.2.x) and VMware NAT (192.168.x.x) subnets.
- **`net` shell command**: registered in `commands[]` and visible in `help`. Bare `net` prints an aggregate overview (interface + ARP cache + UDP socket summary); `net ping <ip>` / `net ifconfig` / `net netstat` forward to the existing implementations; unknown subcommands print usage.
- **e1000 82545EM support (0x100F)**: VMware's default virtual NIC (82545EM) reports device ID 0x100F, which `e1000_init()` and `netif_probe()` did not match. Both now accept 0x100E (82540EM, QEMU default) and 0x100F.
- **`pit_uptime_us()`**: public uptime accessor on top of the interrupt-independent polling clock used by the boot-log timestamps; used by the DHCP wait loops.
- **`tools/pcap_dump.py`**: QEMU `-object filter-dump` pcap decoding helper used to debug the DHCP/RTL8139 bring-up (frame hex dumps, DHCP option breakdown).

### Fixed
- **RTL8139 transmitted correctly-shaped but all-zero frames (CRITICAL)**: the PCI Command register was left with the Bus Master bit clear, so QEMU kept `pci_set_master(false)` and the device's DMA address space was unmapped — every TX descriptor fetch read zeros. `rtl8139_init()` now sets `IO Space + Bus Master` (`cmd |= 0x0005`) before configuring the NIC.
- **RTL8139 never received packets — CAPR register misread**: the RX read pointer is `CAPR + 0x10` (with `RxBufPtr=0` the register reads 0xFFF0, per datasheet); the code used the raw register value as the ring offset, so headers decoded as garbage. Ring offset now computed as `(CAPR + 0x10) % RX_BUF_SIZE`.
- **DHCP deadlocked before interrupts were enabled**: boot-time DHCP runs with IF=0, so the RX IRQ handler never fired and the OFFER/ACK sat unread in the RX ring. The ring-drain logic was extracted into `rtl8139_rx_poll()` (also exported via `g_netif.poll`) and is invoked from the UDP socket wait loops, making packet reception work regardless of interrupt state.
- **Compiler reordered `memcpy` below the device kick under `-O2` (DMA corruption)**: the port-I/O accessors in `io.h` carried no `"memory"` clobber, so GCC was free to sink ordinary stores (e.g. copying a packet into the DMA TX buffer) below the `outb`/`outl` that triggers the transfer — the hardware DMA'd half-written or zeroed buffers. All `inb`/`inw`/`ind`/`outb`/`outw`/`outd` now carry the clobber as an explicit ordering barrier.
- **IPv4 receive dropped DHCP replies**: during negotiation `iface->ip` is still 0.0.0.0, but `ipv4_receive()` filtered on destination address and discarded the OFFER/ACK (often unicast to the granted address). The filter is now skipped while the interface has no address. `ipv4_transmit()` also sends IP broadcast (255.255.255.255) straight to `ff:ff:ff:ff:ff:ff` instead of attempting an ARP lookup for it.

### Changed
- Version strings bumped to 2.8.0 (boot banner, `version` command, GUI title bar).

### File Changes
- `include/net/dhcp.h`, `src/kernel/net/dhcp.c`: new DHCP client (`dhcp_autoconfig()`)
- `src/kernel/net/rtl8139.c`, `include/net/rtl8139.h`: PCI Bus Master enable, CAPR +0x10 fix, `rtl8139_rx_poll()` extraction + `g_netif.poll` registration
- `src/kernel/net/e1000.c`, `include/net/e1000.h`: 82545EM (0x100F) probe
- `src/kernel/net/netif.c`: probe matches 0x100E / 0x100F
- `src/kernel/net/ipv4.c`: DHCP-phase destination filter, IP broadcast MAC shortcut
- `src/kernel/core/main.c`: DHCP-first auto-configuration with static fallback, version bump
- `include/drivers/io.h`: `"memory"` clobbers on all port-I/O accessors
- `src/kernel/shell/shell.c`: `net` command + registration, version bump
- `src/kernel/timer/pit.c`, `include/timer/pit.h`: `pit_uptime_us()`
- `Makefile`: build `dhcp.c`
- `tools/pcap_dump.py`: pcap decoding helper (new)

### Notes
- Regression-verified in headless QEMU (RTL8139, slirp): `net: DHCP ok` / `net: configured` appear ~0.7 s into boot with the offered 10.0.2.15/24, gateway 10.0.2.2; `exec /bin/hello.bin` still completes the full Ring 3 lifecycle (output + exit + shell recovery); `net` and `net ifconfig` show the DHCP-leased configuration and a valid ARP cache.

## [2.7.1] - 2026-08-28
This is a hardening and bug-fix release: it closes the user-pointer validation hole in the system-call interface, makes Ring 3 faults kill the offending process instead of hanging the whole machine, fixes a shell freeze triggered by failed `exec`, fixes a PMM exhaustion bug on 256 MiB machines (and large-RAM VMware configurations), and fixes boot-log timestamps being stuck at `[0.000000]` until SMP init.

### Fixed
- **Missing user-pointer validation in syscalls (SECURITY, ring3 → kernel)**: `sys_write`, `sys_read`, and `sys_puts` dereferenced user-supplied pointers as kernel pointers. A user program could pass any kernel address to read kernel memory (`sys_puts`), write attacker data into kernel memory (`sys_write`), or dump keyboard input over kernel structures (`sys_read`). New `process_check_user_range()` ([process.c](src/kernel/core/process.c)) validates that a buffer lies entirely inside the calling process's code or stack region, with per-page presence checks via `vmm_get_phys()`; all three syscalls now reject invalid pointers with `-EFAULT`-style `-1`, and `sys_puts` scans for NUL page-by-page instead of unbounded `strlen`.
- **User-mode CPU exception froze the entire machine (CRITICAL)**: `isr_handler` halted unconditionally on every fault, so a bug in any user program (e.g. touching an unmapped address) dead-booted the OS. The handler now checks `frame->cs & 3 == 3` and kills the offending process: new `process_kill_current()` marks it ZOMBIE, drops it from `current_process`, and returns the scheduler's kernel-main frame RSP; `isr_common_stub` gained the same `mov rsp, rax` frame-switch hook as the IRQ stub. Kernel-mode exceptions still halt (they are kernel bugs).
- **`exec` failure left the shell permanently frozen**: `process_create()` set `state = READY` before allocating any resources; on failure (out of PMM pages) it returned `-1` leaving a half-initialized READY process behind, so `process_any_active()` stayed true and the shell input loop sat in `hlt` forever ignoring the keyboard (required power-cycle). State is now set only after every resource succeeds, failures unwind via `process_release_memory()` (unmap mapped VAs, free physical pages) and reset the slot, and each failure point prints a specific reason (`no free process slot` / `PMM out of pages (code|stack)`) on screen.
- **PMM exhaustion with 256 MiB RAM (also affected VMware with large RAM)**: the kernel heap arena `[2 MiB, 256 MiB)` swallowed all of RAM when the machine had exactly 256 MiB, leaving `pmm_alloc_page()` with zero free pages — `exec` failed with "PMM out of pages". The arena is now capped at 64 MiB (`[2 MiB, 66 MiB)`) in both the default and largest-region paths of `memory_init()`; `USER_CODE_BASE` (256 MB) remains well above the heap top.
- **Boot-log timestamps stuck at `[0.000000]` until `[init] smp_init done`**: two stacked causes — `pit_init()` ran late in the init sequence, and with interrupts disabled (`sti` only at the very end) `pit_ticks` never advanced. `pit_init(100)` now runs immediately after `serial_init()`, `pic_enable_irq(0)` moved after `interrupts_init()` (which masks all IRQs on completion), and `pit_elapsed_us()` was rewritten as a pure polling timer (reads the PIT countdown register with wraparound detection, `cli/popfq`-guarded) so timestamps are monotonic and continuous across the `sti` boundary regardless of interrupt state.

### Added
- **PMM boot diagnostics**: after the heap reservation, `memory_init()` logs the real free-page count (`[pmm] after heap reserve: free pages = N / 1048576`) and a `WARNING` when no pages remain — environment-specific memory failures (small RAM, different BIOS e820 layouts) are now visible at boot instead of surfacing later as `exec` failures.

### Changed
- Kernel heap arena capped at 64 MiB (was: up to 256 MiB / the largest usable memory region).
- Removed `[fs] i=xxx child=xxx` per-entry debug logging from directory creation (`fs.c`).
- Version strings bumped to 2.7.1 (boot banner, `version` command, GUI title bar).

### File Changes
- `src/kernel/core/process.c`: `process_check_user_range()` (per-page user-buffer validation), `process_kill_current()` (ring3 fault kill path), failure-safe `process_create` (late READY, `goto fail` unwind + slot reset, on-screen failure reasons)
- `include/core/process.h`: declarations for the two new process functions
- `src/kernel/core/syscall.c`: pointer validation in `sys_write` / `sys_read` / `sys_puts`
- `src/kernel/core/isr.asm`: `isr_common_stub` frame-switch hook (`mov rsp, rax`)
- `src/kernel/core/isr.c`: ring3 exception handling in `isr_handler`
- `src/kernel/mm/memory.c`: 64 MiB heap cap (both branches), post-reserve PMM statistics + zero-free WARNING
- `src/kernel/timer/pit.c`: polling-based `pit_elapsed_us()` independent of interrupt state
- `src/kernel/core/main.c`: early `pit_init`, `pic_enable_irq(0)` ordering, version string
- `src/kernel/sched/scheduler.c` / `include/sched/scheduler.h`: `scheduler_main_return_rsp()` accessor used by the fault-kill path
- `src/kernel/fs/fs.c`: debug-log cleanup

### Notes
- Regression-verified in headless QEMU at 256 MiB and 512 MiB: PMM reports ~48608 / ~114144 free pages after the heap reserve, and `exec /bin/hello.bin` completes the full lifecycle (code mapped → run → `SYS_EXIT` → shell recovery) with zero exceptions.
- Known cosmetic limitation: log timestamps can under-count by a few ms across gaps larger than one PIT period (10 ms) — acceptable for boot logs.
- Remaining from the security audit (future work): huge-page split marks whole 2 MiB identity window `USER|WRITABLE` (`vmm_map_page`), `load_user_program` header bounds check, page-table page reclamation on exec, `memory_init` identity-map coverage check for >2 GiB machines.

## [2.7.0] - 2026-08-28
This release completes the system-call and process-scheduling closed loop: user programs now run in Ring 3, issue `int 0x80` system calls, time-share the CPU with the kernel shell via IRQ0 round-robin, and exit cleanly back to the shell. `exec /bin/hello.bin` is verified end-to-end (output + exit + shell recovery) with an automated headless QEMU harness.

### Added
- **Ring 3 user program execution**: `exec /bin/hello.bin` creates a process (PID slot, user code at 256 MB, user stack), maps code/stack pages with the `VMM_USER` bit propagated through PML4/PDPT/PD/PT, sets the TSS kernel stack, and `iretq`s into Ring 3.
- **`int 0x80` system call interface** (`isr.asm` `syscall_entry` + `syscall.c`): saves/restores all user registers, maps arguments (rax=num, rbx/rcx/rdx/r8/r9/r10=args), and writes the return value directly into the saved-rax stack slot. Dispatch table supports registration; implemented: `exit`, `read`, `write`, `getpid`, `yield`, `puts`, `getchar`, `putchar` (`open`/`close` reserved, return `-ENOSYS`).
- **User/kernel time-slice scheduling**: while a user process is RUNNING, IRQ0 alternates between the parked user frame and the kernel main task frame (`scheduler_tick()` park/resume). `sys_yield()` gives up the remainder of a slice via a software `int $0x20`.
- **Process exit closed loop**: `sys_exit` → `process_exit` (atomic w.r.t. IRQ0) → `scheduler_request_main_switch()` → shell resumes. Zombie processes' user pages are reclaimed by `process_reap_zombies()`; the shell reaps zombies before each `exec`.
- **`scheduler_set_main_return()`**: before jumping to user mode, a fresh kernel-main frame (`shell_run` on the scheduler stack) is parked so process exit lands back in a working shell instead of a stale boot-stack snapshot.
- **Embedded user program `/bin/hello.bin`**: built with freestanding `-mcmodel=small` flags, linked via `user/user.ld`, embedded into the kernel image with `incbin` (`user_blob.o`), and installed into the filesystem at boot (`user_programs_install()`).

### Fixed
- **#GP(0x244) on syscall return (CRITICAL)**: `syscall_entry` did `add rsp, 16` after `call syscall_dispatcher`, but `ret` already pops the return address — the extra 8 bytes desynchronized the stack so the final `iretq` loaded the user's RFLAGS slot as CS. Fixed to `add rsp, 8`.
- **User/kernel syscall number mismatch**: kernel enum has `OPEN`/`CLOSE` placeholders, making `SYS_YIELD=6` and `SYS_PUTS=7`; the user program sent 6 and silently invoked `sys_yield`. `hello.c` now matches the kernel enum and documents the numbering.
- **Triple fault on `exec`**: synthetic task frames in `setup_task_stack()` now include valid `SS:RSP` (a 5-word `iretq` pop previously loaded `RSP=0/SS=0`), and `process_run()` raises `cli` before marking the process RUNNING so no IRQ0 tick can park the half-built transition frame.
- **Kernel heap corruption by PMM**: the heap arena is now reserved with `pmm_mark_region()` during `memory_init()`, so physical page allocation can no longer hand out heap pages and destroy heap metadata.
- **User address-space layout**: `USER_CODE_BASE` moved to 256 MB, above the kernel heap arena, eliminating VA collisions.
- **Syscall argument register mapping**: `xchg rcx, rdx` plus `push r10` now correctly delivers (num, arg0..arg5) to the SysV-ABI dispatcher.

### Changed
- Version strings bumped to 2.7.0 (boot banner, `version` command, GUI title bar).
- Removed per-tick/per-syscall debug logging from scheduler, process creation, and syscall dispatcher (kept: one line per process run/exit, ENOSYS warnings).

### File Changes
- `src/kernel/core/isr.asm`: `syscall_entry` stack fix (`add rsp, 8`)
- `src/kernel/core/syscall.c`: dispatcher without per-call logging
- `src/kernel/core/process.c`: `cli`-guarded `process_run`, `scheduler_set_main_return(shell_run)` before `jump_to_user`, debug-log cleanup
- `src/kernel/sched/scheduler.c`: user/main frame alternation, `main_switch_requested` exit path, debug-log cleanup
- `src/kernel/core/main.c`: version string
- `src/kernel/shell/shell.c`: version strings, zombie reaping before `exec`
- `user/hello.c`: corrected syscall numbers, documented kernel enum mapping
- `Makefile`: user program build (freestanding flags, `user_blob` embedding)

### Notes
- Verified with automated headless QEMU (QMP key injection + screendump + `-d int` trace): `exec /bin/hello.bin` prints `Hello from user mode!` in Ring 3, exits via `SYS_EXIT`, and the shell prompt returns; zero exceptions in the interrupt trace.
- `SYS_READ`/`SYS_GETCHAR` currently block in the kernel (`hlt` polling); a blocking-with-schedule implementation is a future step.

## [2.6.0] - 2026-08-27
This release overhauls the VGA graphics subsystem: mode 13h / GUI now work on strict hardware emulators (VMware, VirtualBox) in addition to QEMU, exiting graphics modes no longer corrupts the BIOS font, and colors are deterministic. Also includes GUI shell usability fixes and a graphics-mode regression test harness.

### Fixed
- **Full-screen garbage "dot matrix" after exiting graphics mode (CRITICAL)**: mode 13h repaints spread writes across all four VRAM planes through chain4 addressing, destroying the BIOS character generator in plane 2 and mixing character/attribute data. `vga_set_mode_13h()` now snapshots the font plane (16 KB), the DAC palette (768 B), and the sequencer/graphics registers (SR2, SR4, GR3, GR5, GR6, GR4, GR8) before switching; `vga_set_text_mode()` restores everything exactly. This also fixes text-mode addressing (odd/even) that previously had a hardcoded `SR4` value breaking char/attr plane separation.
- **Graphics mode black screen on VMware / strict VGA implementations**: the mode switch never programmed the Attribute Controller mode registers. `vga_set_mode_13h()` now sets `AC10=0x41` (graphics attribute path), `AC11=0x00` (overscan), `AC12=0x0F` (color plane enable); `vga_set_text_mode()` restores `AC10=0x0C` plus the same registers for text.
- **Wrong colors in graphics mode (white rendered bluish, yellow muddy)**: the DAC palette was never programmed, leaving BIOS text-mode remapped entries. Graphics mode now programs a deterministic standard EGA 16-color palette; text mode restores the snapshotted BIOS palette.
- **GUI desktop could not be exited**: the desktop event loop is now breakable with `ESC`, restores text mode and the text terminal cleanly, and a `desktop_active` guard prevents nested `gfx`/`gui` sessions.
- **Mouse cursor artifacts**: cursor draw/erase is bounded by `CURSOR_W/H` with unified edge clamping; the old position is erased before redrawing and all screen updates erase the cursor first, eliminating trails and trampled pixels.
- **GUI terminal input loss**: typed characters go through the terminal cell buffer (`term_gui_type_char` / `term_gui_backspace`) instead of direct pixel drawing, so input survives screen redraws; window title is preserved across re-renders.
- **`vga_wait_vsync()` hang risk**: retrace polling is now bounded (~200k iterations per phase) so a non-toggling 0x3DA status register can no longer hang the caller.
- **Memory progress bar overflow in GUI sysmon**: fill width is clamped to the bar's inner width.
- **Robustness of drawing calls**: all graphics primitives validate the graphics-mode state and bounds (`vga_is_graphics()`, `vga_plot_pixel()` clamping), preventing text-mode VRAM corruption.
- **klog no longer writes to VRAM while in graphics mode**, avoiding stray pixels in GUI/gfx modes.
- **Font data multiple-definition and scattered extern declarations**: the 8x8 font moved from `88front.h` to a new `font_8x8.c` translation unit; handwritten `extern` prototypes replaced with proper header includes (`isr.c`, `process.c`, `memory.c`).

### Added
- **VGA state snapshot/restore engine** in `vga.c`: saves/restores font plane 2, full DAC (256 entries), and the sequencer/graphics registers touched during plane access — the core mechanism making mode switching reversible.
- **`gfx` command exits with `ESC`** in addition to `q`.
- **KEY_* key codes centralized** in `keyboard.h` (ESC, arrows, etc.) instead of magic numbers in shell code.

### Changed
- Version strings bumped to 2.6.0 (boot banner, `version` command, GUI header).

### File Changes
- `src/kernel/drivers/vga.c`: snapshot/restore engine (font plane, DAC, registers), AC10/11/12 programming, EGA palette, bounded vsync, bounds-checked drawing
- `src/kernel/drivers/font_8x8.c`: new font data translation unit (moved out of `88front.h`)
- `include/gfx/88front.h`: font data replaced by extern declaration
- `include/drivers/vga.h`: `vga_is_graphics()`, `vga_wait_vsync()`, `vga_puthex()` declarations
- `include/drivers/keyboard.h`: centralized KEY_* codes
- `src/kernel/shell/shell.c`: ESC-exitable GUI/gfx, desktop_active guard, cursor management, vsync in event loops, memory bar clamp, version strings
- `src/kernel/shell/terminal.c`: `term_gui_type_char()` / `term_gui_backspace()` buffered input, title-preserving re-render
- `include/shell/terminal.h`: new terminal API declarations
- `src/kernel/drivers/mouse.c`: cursor visibility state, unified edge clamping, erase-before-redraw
- `src/kernel/core/main.c`: klog graphics guard, version string
- `src/kernel/core/isr.c`, `src/kernel/core/process.c`, `src/kernel/mm/memory.c`: replaced handwritten externs with header includes
- `Makefile`: added `font_8x8.c` to driver sources

### Notes
- Verified with an automated headless QEMU harness (QMP-driven key injection + screendump) on both `std` VGA and `cirrus-vga`: color bars correct, clean text mode after exit, GUI renders with correct palette. VMware behavior addressed by the AC register fixes; report remaining differences if any.
- `make run` uses `-nographic`, which has no visible VGA window; use a graphical QEMU/VMware session to see `gfx`/`gui`.

## [2.5.1] - 2026-08-27
 This release focuses on debugging, crash-safety, and robustness improvements: it adds a RAM-disk fallback when no ATA disk is present, halts on CPU exceptions to avoid fault loops, enables debug symbols, adds extensive boot/filesystem tracing, and configures the GRUB menu.

### Added
- **RAM disk fallback**: `disk.c` now allocates an in-memory RAM disk when no ATA disk is detected (e.g. QEMU without `-hda`), routing `disk_read_sector()` / `disk_write_sector()` through it so the filesystem can still initialize and format. Prevents a hard boot failure on diskless environments.
- **GRUB menu configuration**: `grub.cfg` now sets `timeout=2` and `default=0` for automated/bootable image selection.
- **Debug symbols in build**: `Makefile` CFLAGS now include `-g` to emit DWARF debug info for `objdump`-based analysis.
- **`find_call.py`**: new helper script that disassembles `build/kernel.bin` with `objdump` and locates `call` instructions to `strcmp` within a target address range (useful for tracing filesystem string comparisons).
- **Extensive boot & filesystem tracing**: added `klog()` diagnostics throughout `kernel_main()` (one per subsystem init) and across the FAT32 layer — `fs_init()`, `fat_alloc_cluster()`, `fs_check_entry_exists()`, `fs_create_dir()`, and `fs_load_directory()` now log their progress, child pointers, and resolved entry names.

### Fixed
- **CPU exception infinite fault loop**: `isr.c` now executes `hlt` after printing an unhandled exception, halting the CPU instead of spinning on repeated fault/print cycles.
- **Filesystem debug logging safety**: `fs_check_entry_exists()` now bounds-checks each child's `name` and sanitizes non-printable bytes before logging, avoiding garbage reads while enumerating directory children.

### File Changes
- `Makefile`: added `-g` debug flag to CFLAGS
- `find_call.py`: new disassembly/ call-tracing helper script
- `grub.cfg`: added `timeout` and `default` settings
- `src/kernel/core/isr.c`: halt on CPU exception
- `src/kernel/core/main.c`: added per-subsystem init tracing
- `src/kernel/drivers/disk.c`: RAM disk fallback when no ATA disk present
- `src/kernel/fs/fs.c`: debug-only tracing and safer name logging across FS operations

### Notes
- Most additions in this release are debug/observability aids (kernel `klog` output and `find_call.py`). These are intended for diagnosis and do not change runtime behavior of the production paths beyond the RAM-disk and exception-halt fixes.

## [2.5.0] - 2026-07-24
This release introduces foundational user mode (Ring 3) support, enabling the kernel to run user programs in a protected environment with system call interfaces.

### Added
- **User mode (Ring 3) support**: complete infrastructure for executing user programs outside kernel space
  - GDT entries for user code (0x18) and data (0x20) segments with DPL=3
  - TSS (Task State Segment) for privilege level transitions
  - `iretq`-based kernel-to-user transition via `jump_to_user()`
- **System call framework**: `int 0x80`-based syscall interface accessible from Ring 3
  - Initial syscalls: `SYS_EXIT`, `SYS_WRITE`, `SYS_GETPID`, `SYS_PUTS`
  - Extensible syscall table with runtime handler registration
- **Process management foundation**: basic process control block and lifecycle
  - Process table with PID allocation
  - User memory layout: code at 4MB, data at 8MB, stack at ~2GB
  - Program loader skeleton for flat binary format
- **User program build system**: cross-compiler toolchain for user-space programs
  - Separate linker script (`user/user.ld`)
  - Test program: `user/hello.c`

### Technical Details
- **Memory layout**:
  - User code base: `0x00400000` (4 MiB)
  - User data base: `0x00800000` (8 MiB)
  - User stack top: `0x7FFFF000` (~2 GiB, grows down)
  - Stack size: 64 KiB default
- **GDT layout** (expanded from 5 to 8 entries):
  - 0x00: Null descriptor
  - 0x08: Kernel code (64-bit, DPL=0)
  - 0x10: Kernel data (DPL=0)
  - 0x18: User code (64-bit, DPL=3)
  - 0x20: User data (DPL=3)
  - 0x28: TSS descriptor (16 bytes)

### File Changes
- `include/core/gdt.h`: expanded GDT with TSS descriptor support
- `src/kernel/core/gdt.c`: GDT initialization with `gdt_set_tss()`
- `include/core/tss.h`: TSS structure definitions
- `src/kernel/core/tss.c`: TSS initialization and kernel stack management
- `include/core/process.h`: process control block and loader interface
- `src/kernel/core/process.c`: process management implementation
- `include/core/syscall.h`: syscall interface definitions
- `src/kernel/core/syscall.c`: syscall dispatcher and handlers
- `src/kernel/core/isr.asm`: syscall entry point via `int 0x80`
- `src/kernel/core/isr.c`: IDT gate setup with DPL=3 for syscall
- `user/hello.c`: test user program
- `user/user.ld`: user program linker script
- `user/Makefile`: user program build system

### Notes
- User mode is now ready for testing. Next steps include:
  - Implementing more syscalls (file operations, memory allocation)
  - Adding proper process scheduling for user programs
  - Creating shell commands to load and execute user programs

## [2.4.2] - 2026-07-24
 This release fixes critical memory management bugs that caused kernel crashes (Triple Fault) on VirtualBox.
## Fixed
 - **VMM huge page corruption (CRITICAL)**: `vmm_map_page()` no longer overwrites existing 2 MiB huge page entries with zero. Previously, this would corrupt kernel identity mappings, causing Page Fault → Double Fault → Triple Fault when executing code in affected regions. Huge pages are now preserved; if a 4 KiB mapping is requested in a huge-page region, the function returns early.
 - **pit_format_time buffer overflow**: capped seconds value to 99999 (~27 hours at 100 Hz tick) and added boundary check in the digit extraction loop to prevent stack corruption if `pit_ticks` overflows or is corrupted. Reduced `sec_str` buffer from 16 to 8 bytes.
## Security
 - Both fixes address potential kernel instability or crashes that could be triggered by long uptime or ACPI memory mapping operations.
## File Changes
 - `src/kernel/mm/memory.c`: fixed `vmm_map_page()` huge page handling
 - `src/kernel/timer/pit.c`: added overflow protection to `pit_format_time()`
## [2.4.1] - 2026-06-29
 This release focuses on fixing critical and medium-priority bugs in the FAT32 filesystem implementation, improving data consistency, directory persistence, and error handling.
## Fixed
 - **fs_load_directory recursive loading**: subdirectories are now recursively loaded into memory on boot, ensuring their contents remain visible after a reboot
 - **fs_write_directory_entry FAT corruption**: fixed incorrect FAT chain update during multi-cluster directory expansion by tracking `prev_cluster` instead of overwriting the current EOC marker
 - **fs_write_file data consistency**: on write failure, the original file is no longer corrupted. New clusters are allocated and written first; in-memory state and old cluster chain are only updated after all disk writes succeed
 - **fs_save_directory entry offset bug**: fixed incorrect `entry_offset` calculation that skipped entries when NULL gaps existed in the children array
 - **fs_save_directory . and .. preservation**: non-root directories now correctly preserve `.` and `..` entries when saved back to disk
 - **fs_save_directory cluster expansion/truncation**: directory saving now supports allocating new clusters when more space is needed and freeing excess clusters when shrinking
 - **read_directory_entries . / .. filtering**: special FAT32 `.` and `..` entries are now excluded from the children array to prevent slot wastage
 - **fs_delete_entry path support**: `fs_delete_entry()` now uses `resolve_parent_and_name()` to support deleting files and directories via full paths
 - **fs_load Boot Signature validation**: added `0xAA55` boot signature verification to prevent loading corrupted boot sectors
 - **fs_create_file/dir orphan entries**: memory is now allocated before writing disk directory entries. On allocation failure, no orphan directory entries are left on disk
 - **fat_read_entry I/O error handling**: `fat_read_entry()` now returns `FAT32_IO_ERROR` (0xFFFFFFFF) on I/O failure instead of 0, and all cluster chain traversals have been updated to safely abort on this error value
## File Changes
 - `src/kernel/fs/fs.c`: comprehensive fixes for directory loading, saving, file writing, entry deletion, creation, and FAT I/O error handling
## [2.4.0] - 2026-06-28
 This release restores the network subsystem, fixes critical PIC and ACPI bugs, and adopts Linux-style boot messages with live PIT timestamps.
## Added
 ### Network Stack (Restored)
 - Re-introduced full network stack: `netif`, Ethernet, ARP, IPv4, ICMP, UDP
 - Intel E1000 and Realtek RTL8139 NIC drivers
 - Shell network commands: `ping`, `ifconfig`, `netstat`
 - QEMU `run` target now includes `-netdev user,id=net0 -device rtl8139,netdev=net0`
 ### Kernel Boot Logging
 - `klog()`: unified kernel logger that prints timestamped messages to both VGA and serial
 - Linux-style `[    sec.usec] subsystem: message...` format for all boot messages
 ### Live Timestamps
 - `pit_ticks`: 64-bit global tick counter incremented on every IRQ0
 - `pit_format_time()`: formats ticks into `[    sec.usec]` string
 - Boot messages after PIT initialization now display real elapsed time instead of `0.000000`
## Fixed
 - **Double EOI corruption**: removed fallback `pic_send_eoi()` for non-zero IRQs in `irq_handler()`; each handler now sends EOI exactly once, preventing PIC state corruption on VirtualBox
 - **ACPI 64-bit address truncation**: `power.c` now uses temporary virtual mapping via `vmm_map_page()` to safely read ACPI tables located above 4 GiB
## Changed
 - All boot initialization messages in `main.c`, `pci.c`, and `smp.c` migrated to `klog()` with automatic timestamps; manual `[    0.000000]` prefixes removed
## File Changes
 - `src/kernel/core/isr.c`: IRQ0 increments `pit_ticks`; removed fallback EOI for non-zero IRQs
 - `src/kernel/core/main.c`: converted all init messages to `klog()`
 - `src/kernel/core/smp.c`: converted SMP messages to `klog()`
 - `src/kernel/drivers/pci.c`: converted PCI scan message to `klog()`
 - `src/kernel/drivers/mouse.c`: added explicit `pic_send_eoi()` in mouse handler
 - `src/kernel/drivers/power.c`: added `acpi_temp_map()` for safe 64-bit ACPI table access
 - `src/kernel/timer/pit.c/h`: added `pit_ticks` and `pit_format_time()`
 - `include/drivers/vga.h`: declared external `klog()`
 - `src/kernel/shell/shell.c`: added `ping`, `ifconfig`, `netstat` commands
 - `Makefile`: added network sources and QEMU netdev options
 - `include/net/*`, `src/kernel/net/*`: restored network subsystem headers and drivers
## [2.3.0] - 2026-06-26
 This release focuses on driver-level improvements: SMP multicore support, ATA DMA, PC speaker audio, and critical ACPI fixes.
## Added
 ### SMP / Multicore Support
 - AP (Application Processor) trampoline (`ap_trampoline.asm`) for 16-bit real mode to 64-bit long mode transition
 - MADT (Multiple APIC Description Table) parser to detect available CPU cores via ACPI
 - `smp_init()`: initializes and starts all AP cores using INIT-SIPI-SIPI sequence via LAPIC ICR
 - `ap_main()`: AP entry point that reloads GDT/IDT and enters idle halt loop
 - `smp_get_cpu_count()`: returns the number of online CPUs
 ### ATA DMA (Bus Master IDE)
 - Bus Master IDE DMA support for ATA disk reads with automatic fallback to PIO
 - PRDT (Physical Region Descriptor Table) setup for single-sector DMA transfers
 - `ata_read_sector_dma()`: performs disk reads via DMA engine instead of CPU-driven PIO
 ### PC Speaker Audio
 - New speaker driver (`speaker.c` / `speaker.h`) using PIT channel 2 square wave generation
 - `speaker_play(uint32_t freq_hz)`: plays a tone at the specified frequency
 - `speaker_stop()`: disables the PC speaker gate
 - `speaker_beep(uint32_t freq_hz, uint32_t duration_ms)`: synchronous beep with PIT-based delay
 - `pit_delay_ms()`: precise millisecond delay using PIT counter reads
 ### ACPI Infrastructure
 - Public `acpi_find_table(const char* sig)` exposed via `power.h` for external ACPI table lookup
## Fixed
 - **XSDT pointer truncation** in `power.c`: 64-bit XSDT addresses were truncated to 32-bit, causing ACPI table lookup failures on systems with tables above 4 GiB. Now properly reads the 64-bit extended field and skips entries beyond identity-mapped range.
## Changed
 - `gdt_reload()` and `idt_reload()`: new functions to reload GDT/IDT without reinitializing entries, used by AP cores
 - `gdt[]`, `gdt_ptr`, `idt[]`, `idt_ptr`: removed `static` linkage so AP trampoline and SMP code can reference them
## File Changes
 - `src/kernel/core/ap_trampoline.asm`: new AP startup trampoline
 - `src/kernel/core/smp.c`, `include/core/smp.h`: SMP initialization and AP management
 - `src/kernel/core/gdt.c`, `include/core/gdt.h`: exposed GDT symbols and added reload function
 - `src/kernel/core/idt.c`, `include/core/idt.h`: exposed IDT symbols and added reload function
 - `src/kernel/drivers/disk.c`: added Bus Master IDE DMA implementation
 - `src/kernel/drivers/speaker.c`, `include/drivers/speaker.h`: new PC speaker driver
 - `src/kernel/timer/pit.c`, `include/timer/pit.h`: added `pit_delay_ms()`
 - `src/kernel/drivers/power.c`, `include/drivers/power.h`: fixed XSDT 64-bit handling, exposed `acpi_find_table()`
 - `Makefile`: added AP trampoline binary embedding via `objcopy`
## [2.2.0] - 2026-06-26
 This release introduces a unified terminal abstraction layer and a GUI terminal emulator, replacing the legacy GUI shell with a proper text buffer renderer.
## Added
- Terminal abstraction layer (`terminal.h` / `terminal.c`) providing a unified interface for text and graphical terminals
- GUI terminal emulator with a character grid buffer (35x19 cells) supporting `\n`, `\b`, `\t`, scrolling, and color attributes
- `term_gui_render()`: renders the terminal buffer to the VGA graphics canvas
- `term_gui_get_cursor_y()`: returns the pixel Y coordinate of the current cursor row for input positioning
- Real-time date/time display in the GUI footer bar (RTC-based, updates every second)
## Changed
- GUI Shell now shares the same `execute_command()` logic as the text shell via the terminal abstraction layer
- Command output inside the GUI is captured by the terminal buffer instead of direct VGA drawing, preventing overlap with user input
- Footer bar text changed from `"Press 'q' to return to text mode"` to live clock display
- Updated GUI screenshots (`assets/shellgui.png`, `assets/mew.png`)
## Removed
- Pressing `q` in GUI mode no longer returns to text mode; GUI is now the persistent desktop environment
## Fixed
- User input no longer overlaps previous command output in the GUI shell; the input prompt follows the terminal cursor
## [2.1.0] - 2026-06-26
 This release introduces physical and virtual memory management alongside kernel panic and assertion facilities, laying the groundwork for a robust 64-bit memory subsystem.
## Added
 ### Physical Memory Manager (PMM)
  - Bitmap-based physical page frame allocator managing the first 4 GiB of RAM
  - `pmm_init(uint64_t mb_info_phys)`: parses Multiboot2 memory map tag to discover available RAM regions
  - `pmm_alloc_page()`: allocates a single 4 KiB physical page
  - `pmm_alloc_pages(size_t count)`: allocates contiguous physical pages
  - `pmm_free_page()` / `pmm_free_pages()`: releases physical pages back to the bitmap
  - Automatic reservation of kernel image, page bitmap, low 2 MiB, and non-RAM regions
 ### Virtual Memory Manager (VMM)
  - Simple 4-level page table manipulation on top of the existing identity-mapped address space
  - `vmm_init()`: discovers current PML4 from CR3
  - `vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)`: maps a 4 KiB virtual page, allocating intermediate PDPT/PD/PT tables on demand via PMM
  - `vmm_unmap_page(uint64_t virt)`: removes a virtual mapping
  - `vmm_get_phys(uint64_t virt)`: translates a virtual address to its physical counterpart (supports both 4 KiB and 2 MiB huge pages)
  - `vmm_reload_cr3()`: flushes TLB after page table modifications
 ### Kernel Panic & Assert
  - `panic(const char* msg, const char* file, int line)`: disables interrupts, prints panic details to serial COM1 and VGA console, then halts the CPU
  - `panic_assert(const char* cond, const char* file, int line)`: assertion failure wrapper
  - `PANIC(msg)` and `ASSERT(cond)` macros providing `__FILE__` and `__LINE__` automatically
## Changed
  - `kernel_main` now accepts `uint64_t mb_info_phys` to receive the Multiboot2 info structure from the bootloader
  - Boot sequence extended with `[PMM]` and `[VMM]` initialization stages before heap setup
  - `linker.ld` exports `kernel_start` and `kernel_end` symbols for accurate kernel image reservation
## File Changes
  - `include/mm/memory.h`: added PMM, VMM, and panic/assert declarations
  - `src/kernel/mm/memory.c`: implemented PMM bitmap allocator, VMM page table walker, and panic output routines
  - `src/kernel/core/main.c`: updated boot flow to initialize PMM/VMM and pass Multiboot2 pointer
  - `linker.ld`: added `kernel_start` / `kernel_end` linker symbols
## [2.0.1] - 2026-06-26
 This release completes the architecture migration from 32-bit to 64-bit x86-64, upgrading the bootloader to GRUB2 with Multiboot2 protocol and enabling long mode.
## Added
 ### 64-bit Architecture Support
  - Full x86-64 long mode support with 4-level page tables (PML4 -> PDPT -> PD -> PT)
  - Identity mapping for the first 4 GiB of physical memory
  - 64-bit GDT and IDT structures with proper long-mode segment descriptors (L=1, D=0)
  - 64-bit interrupt handling with explicit register push/pop and `iretq`
  - System V AMD64 ABI compliant scheduler context switching
 ### Bootloader Upgrade
  - Migrated from 32-bit Multiboot1 to GRUB2 + Multiboot2 protocol
  - Proper Multiboot2 header alignment (8-byte boundary) with entry address tag
 ### Build System
  - Updated compiler flags for 64-bit (`-m64`, `-mno-red-zone`, `-mcmodel=large`)
  - NASM output format changed to `elf64`
  - Linker script updated to `elf64-x86-64` architecture
  - Disabled SSE/AVX generation (`-mno-sse -mno-sse2 -mno-mmx`) for kernel compatibility
## Changed
  - All pointer types (`size_t`, `ptrdiff_t`, `physaddr_t`, `virtaddr_t`) upgraded to 64-bit
  - Interrupt frame structure expanded to 64-bit registers
  - Task stack size increased from 16 KiB to 32 KiB
  - Task context switched from `esp` to `rsp`
## Fixed
  - GDT code segment descriptors now correctly set L-bit (long mode) preventing double faults on interrupt entry
  - Multiboot2 header size alignment causing GRUB `unsupported tag: 0x8` error
  - IDT gate offset handling for 64-bit handler addresses
## File Changes
  - 15+ files modified across boot, core, mm, sched, net, and build system
  - `src/boot/boot.asm`: complete rewrite for 64-bit Multiboot2 + long mode entry
  - `src/kernel/core/gdt.c`: fixed 64-bit segment granularity flags
  - `src/kernel/core/idt.c`, `isr.c`, `isr.asm`: 64-bit IDT/ISR overhaul
  - `src/kernel/sched/scheduler.c`: 64-bit context switch implementation
  - `Makefile`: 64-bit toolchain flags and `-mno-sse`
  - `linker.ld`: 64-bit ELF output format
## [1.2.0] - 2026-06-25
 This release completely redesigns the GUI desktop into a TempleOS-style tiling interface with an interactive graphical shell.
## Added
 ### Tiling GUI Desktop
  - TempleOS-style tiling desktop layout with header bar, left menu panel, right content area, and footer
  - Keyboard-driven menu navigation using arrow keys with yellow highlight selection
  - Enter key to switch between function panels (Shell, Files, Edit, Viewer, CATs)
  - 8x8 bitmap font rendering support for GUI text display
 ### Interactive Graphical Shell
  - Fully interactive shell embedded in the right content panel
  - Command input with backspace support and cursor tracking
  - Supported commands: `ls`, `cd`, `mkdir`, `touch`, `pwd`, `help`, `clear`, `version`, `whoami`, `shutdown`
  - Directory listings with color-coded entries (directories in light blue, files in white)
  - Auto-scroll with screen clearing when output exceeds panel bounds
 ### New Assets
  - `assets/shellgui.png`: screenshot of the interactive shell panel
  - `assets/mew.png`: screenshot of the CAT Viewer panel
## Changed
  - Redesigned `gui` command: replaced Windows 98 style window with pure tiling layout
  - Updated version string to v1.2.0 across boot message, GUI header, and shell commands
  - Updated README with GUI Desktop section and screenshots
## File Changes
  - 5 files changed, 470 insertions(+), 13 deletions(-)
  - `include/gfx/88front.h`: new 8x8 bitmap font data
  - `include/drivers/vga.h`: added font and rectangle drawing declarations
  - `src/kernel/drivers/vga.c`: added `vga_draw_rect()`, `vga_draw_string()`, `vga_draw_char()`
  - `src/kernel/shell/shell.c`: complete GUI rewrite with tiling layout, menu nav, and interactive shell
  - `src/kernel/core/main.c`: updated boot version string
## [1.1.0] - 2026-06-25
 This release introduces graphical display support to Kil0yOS, including a VGA graphics mode and a simple desktop environment.
## Added
 ### Graphical Display System
  - VGA Mode 13h (320x200, 256 colors) support via direct hardware register programming
  - `vga_set_mode_13h()`: switch from text mode to graphics mode
  - `vga_set_text_mode()`: restore standard 80x25 text mode
  - `vga_plot_pixel()`: draw individual pixels in graphics mode
  - `vga_fill_rect()`: draw filled rectangles for GUI rendering
  - `vga_draw_color_bars()`: display standard color bar test pattern
 ### New Shell Commands
  - `gfx`: switch to graphical mode and display a standard color bar test pattern, press `q` to return
  - `gui`: launch a simple Windows 98 style desktop with a cyan background and gray taskbar, press `q` to return
## Changed
  - Updated version string from v1.0.5 to v1.1.0 in kernel boot message and shell `version` command
## File Changes
  - 4 files modified, 190 insertions, 2 deletions
  - include/drivers/vga.h: added graphics mode declarations and constants
  - src/kernel/drivers/vga.c: implemented VGA graphics mode, pixel drawing, rectangle fill, and color bars
  - src/kernel/shell/shell.c: added `gfx` and `gui` commands, updated version string
  - src/kernel/core/main.c: updated boot version string
## [1.0.5] - 2026-06-25
 This is a critical maintenance and feature enhancement release focused on fixing core network subsystem issues and improving overall system stability and reliability.
 All users relying on network functionality are strongly recommended to upgrade.
## Added
 ### New shell network commands:
  - net chknic: List all available network interfaces
  - net wire <interface>: Establish wired network connection
  - ping: Test network connectivity
  - Official driver support for Intel PRO/1000 MT (E1000) NIC
  - Debug logging for received packets in E1000 driver
  - Early exit logic for DHCP client once a valid IP address is acquired
## Changed
  - Extended DHCP client waiting loop from 5 million iterations to 20 million iterations
  - Refractored entire project into categorized modular directories for better code maintainability
  - Rewrote ARP busy-wait logic to return error codes and delegate retry logic to callers
## Fixed
 ### Core Network Stack
  - Missing UDP protocol handling in IPv4 packet dispatcher
  - Failure to broadcast DHCP packets
 ### Intel E1000 NIC Driver
  - Fixed interrupt detection logic inside e1000_poll(), now captures all interrupt sources
  - Fixed driver hang caused by invalid MMIO access patterns
 ### Kernel & File System
  - Multiple critical kernel memory safety and stability defects
  - Fixed file system bugs leading to data corruption and system crashes
  - Fixed issues with ACPI shutdown, file system persistence and general driver reliability
### Known Issues
  - Network interrupt handling implementation remains incomplete; polling mode is recommended during heavy network operations
  - Network adapter configuration adjustments may be required under certain virtual machine environments for full connectivity
## File Changes
  - 7 files modified, 224 insertions, 69 deletions
  - include/include/net/e1000.h: E1000 network driver header
  - include/include/net/net.h: Core network stack header
  - src/kernel/net/e1000.c: E1000 driver implementation
  - src/kernel/net/net.c: Core network stack logic
  - src/kernel/net/rtl8139.c: RTL8139 network driver implementation
  - src/kernel/shell/shell.c: Shell built-in network command implementations
  - .gitignore: Git ignore configuration
