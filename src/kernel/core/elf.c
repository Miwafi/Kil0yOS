#include "core/elf.h"
#include "core/uvm.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "drivers/vga.h"

/* --- Minimal ELF64 structures (little-endian, x86-64) ---------------- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf_phdr_t;

#define PT_LOAD   1
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62

static uint64_t page_down(uint64_t a) { return a & ~0xFFFULL; }
static uint64_t page_up(uint64_t a)   { return (a + 0xFFF) & ~0xFFFULL; }

int elf_is_elf64(const uint8_t* data, size_t size) {
    if (size < sizeof(elf_ehdr_t)) return 0;
    return data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
}

/* Copy the file-backed slice of one PT_LOAD. The segment pages live in the
 * process's own page tables, so vmm_get_phys must walk the process root;
 * content is then written through the identity map (VA == PA, CPU CR3
 * stays on the boot tables). Writing user VAs directly would hit the boot
 * tables' 2MB identity huge pages and the copy would silently vanish.
 * BSS pages were zero-filled by uvm_map_range, so only
 * [p_offset, p_offset+p_filesz) needs copying. Returns 0 or ELF_ERR_MAP. */
static int elf_copy_segment(const uint8_t* data, const elf_phdr_t* ph, uint64_t bias) {
    uint64_t vstart = bias + ph->p_vaddr;
    const uint8_t* src = data + ph->p_offset;

    uint64_t done = 0;
    while (done < ph->p_filesz) {
        uint64_t va = vstart + done;
        uint64_t page = page_down(va);
        uint64_t phys = vmm_get_phys(page);   /* walks vmm_pml4 = proc root */
        if (phys == 0) return ELF_ERR_MAP;
        uint64_t off = va - page;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > ph->p_filesz - done) chunk = ph->p_filesz - done;
        memcpy((void*)(phys + off), src + done, chunk);
        done += chunk;
    }
    return 0;
}

static int elf_load_at(process_t* proc, const uint8_t* data, size_t size,
                       uint64_t forced_base, elf_load_result_t* out);

int elf_load(process_t* proc, const uint8_t* data, size_t size,
             elf_load_result_t* out) {
    return elf_load_at(proc, data, size, 0, out);
}

int elf_load_interp(process_t* proc, const uint8_t* data, size_t size,
                    uint64_t base, elf_load_result_t* out) {
    if (base == 0 || (base & 0xFFF)) return ELF_ERR_FORMAT;
    return elf_load_at(proc, data, size, base, out);
}

static int elf_load_at(process_t* proc, const uint8_t* data, size_t size,
                       uint64_t forced_base, elf_load_result_t* out) {
    if (!elf_is_elf64(data, size)) return ELF_ERR_FORMAT;

    const elf_ehdr_t* eh = (const elf_ehdr_t*)data;
    if (eh->e_ident[4] != 2 ||          /* ELFCLASS64 */
        eh->e_ident[5] != 1 ||          /* little-endian */
        eh->e_machine != EM_X86_64 ||
        eh->e_phentsize != sizeof(elf_phdr_t) ||
        eh->e_phnum == 0 ||
        (size_t)eh->e_phoff + (size_t)eh->e_phnum * sizeof(elf_phdr_t) > size) {
        return ELF_ERR_FORMAT;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return ELF_ERR_FORMAT;

    const elf_phdr_t* ph = (const elf_phdr_t*)(data + eh->e_phoff);

    uint64_t bias = 0;
    uint64_t min_vaddr = ~0ULL;
    const char* interp = "";
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            if (ph[i].p_offset + ph[i].p_filesz > size ||
                ph[i].p_filesz == 0 ||
                ph[i].p_filesz >= sizeof(out->interp)) {
                return ELF_ERR_FORMAT;
            }
            interp = (const char*)(data + ph[i].p_offset);
        }
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz > 0 &&
            ph[i].p_vaddr < min_vaddr) {
            min_vaddr = ph[i].p_vaddr;
        }
    }

    if (eh->e_type == ET_DYN) {
        /* Place the lowest segment at the chosen base: static-PIE and
         * dynamic-PIE images alike (the latter is relocated by the
         * interpreter via AT_PHDR). The interpreter itself is placed at
         * its explicit base (its first PT_LOAD is at p_vaddr 0). */
        uint64_t want = (forced_base != 0) ? forced_base : UVM_ELF_BASE;
        bias = want - page_down(min_vaddr);
    } else {
        /* ET_EXEC: linked addresses are absolute. Anything below the
         * kernel heap arena would corrupt it - refuse with a hint. */
        if (min_vaddr < UVM_ELF_BASE || forced_base != 0) return ELF_ERR_LAYOUT;
    }

    uint64_t max_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf_phdr_t* p = &ph[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        if (p->p_offset + p->p_filesz > size) return ELF_ERR_FORMAT;

        uint32_t prot = 0;
        if (p->p_flags & PF_R) prot |= UVM_PROT_READ;
        if (p->p_flags & PF_W) prot |= UVM_PROT_WRITE;
        if (p->p_flags & PF_X) prot |= UVM_PROT_EXEC;
        if (prot == 0) prot = UVM_PROT_READ;

        uint64_t vs = bias + p->p_vaddr;
        uint64_t ve = vs + p->p_memsz;      /* BSS included: mapped + zeroed */
        if (uvm_map_range(proc, vs, ve, prot) != 0) return ELF_ERR_MAP;

        /* Walk the process root while resolving segment pages (see
         * elf_copy_segment); the CPU CR3 itself stays untouched. */
        uint64_t saved_root = vmm_current_root();
        if (proc->cr3) vmm_set_root_ptr(proc->cr3);
        int crc = elf_copy_segment(data, p, bias);
        vmm_set_root_ptr(saved_root);
        if (crc != 0) return ELF_ERR_MAP;

        if (page_up(ve) > max_end) max_end = page_up(ve);
    }

    /* Program headers must be reachable through a loaded segment so the
     * runtime can walk them via auxv AT_PHDR. e_phoff is a FILE offset,
     * so locate the segment whose file range contains it and derive the
     * virtual address through that segment's mapping. */
    uint64_t phdr_va = 0;
    int phdr_ok = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf_phdr_t* p = &ph[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        if (eh->e_phoff >= p->p_offset &&
            eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf_phdr_t)
                <= p->p_offset + p->p_filesz) {
            phdr_va = bias + p->p_vaddr + (eh->e_phoff - p->p_offset);
            phdr_ok = 1;
            break;
        }
    }
    if (!phdr_ok) return ELF_ERR_PHDRS;

    out->entry = bias + eh->e_entry;
    out->brk_start = (max_end > 0) ? max_end : UVM_ELF_BASE;
    out->phdr = phdr_va;
    out->phnum = eh->e_phnum;
    memset(out->interp, 0, sizeof(out->interp));
    if (interp[0]) {
        strncpy(out->interp, interp, sizeof(out->interp) - 1);
    }
    return 0;
}
