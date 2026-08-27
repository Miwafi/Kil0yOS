# Changelog
 All notable changes to this project will be documented in this file.
 The format follows Keep a Changelog and this project adheres to Semantic Versioning.

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
