# Changelog
 All notable changes to this project will be documented in this file.
 The format follows Keep a Changelog and this project adheres to Semantic Versioning.
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