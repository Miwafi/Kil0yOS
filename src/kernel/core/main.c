#include "core/gdt.h"
#include "core/idt.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "mm/memory.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/power.h"
#include "drivers/pci.h"
#include "shell/shell.h"
#include "fs/fs.h"
#include "drivers/device.h"
#include "timer/pit.h"
#include "sched/scheduler.h"
#include "net/net.h"
void kernel_main() {
    vga_init();
    
    vga_set_color(vga_entry_color(COLOR_LIGHT_CYAN, COLOR_BLACK));
    vga_puts("Kil0yOS v1.1.2\n");
    vga_puts("==================================\n");
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));
    
    vga_puts("[GDT] Initializing\n");
    gdt_init();
    vga_puts("[GDT] OK\n");
    
    vga_puts("[IDT] Initializing\n");
    idt_init();
    vga_puts("[IDT] OK\n");
    
    vga_puts("[ISRs] Initializing\n");
    isr_init();
    vga_puts("[ISRs] OK\n");
    
    vga_puts("[PIC] Initializing\n");
    interrupts_init();
    vga_puts("[PIC] OK\n");
    
    vga_puts("[Memory] Initializing\n");
    memory_map_t map = {0};
    memory_init(&map, 1);
    vga_puts("[Memory] OK\n");
    
    vga_puts("[DeviceManager] Initializing\n");
    device_init();
    vga_puts("[DeviceManager] OK\n");
    
    vga_puts("[Filesystem] Initializing\n");
    fs_init();
    vga_puts("[Filesystem] OK\n");
    
    vga_puts("[Shell] Initializing\n");
    shell_init();
    vga_puts("[Shell] OK\n");
    
    vga_puts("[Keyboard] Initializing\n");
    keyboard_init();
    vga_puts("[Keyboard] OK\n");

    vga_puts("[Mouse] Initializing\n");
    mouse_init();
    vga_puts("[Mouse] OK\n");
    
    vga_puts("[Scheduler] Initializing\n");
    scheduler_init();
    vga_puts("[Scheduler] OK\n");
    
    vga_puts("[PIT] Initializing\n");
    pit_init(100);
    vga_puts("[PIT] OK\n");
    
    vga_puts("[Power] Initializing ACPI...\n");
    power_init();
    vga_puts("[Power] OK\n");
    
    vga_puts("[PCI] Initializing\n");
    pci_init();
    vga_puts("[PCI] OK\n");
    
    vga_puts("[Network] Initializing\n");
    net_init();
    vga_puts("[Network] OK\n");
    
    vga_puts("\nWelcome!\n");
    vga_puts("Type 'help' for available commands.\n\n");
    
    enable_interrupts();
    
    shell_run();
}