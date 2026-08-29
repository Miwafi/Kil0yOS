#include "core/uvm.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "drivers/vga.h"

/* PTE attribute bits for a user protection set. Non-executable writable
 * pages get NX so stray jumps into data fault immediately. */
static uint64_t prot_to_pte(uint32_t prot) {
    uint64_t f = VMM_PRESENT | VMM_USER;
    if (prot & UVM_PROT_WRITE) f |= VMM_WRITABLE;
    if (!(prot & UVM_PROT_EXEC)) f |= VMM_NX;
    return f;
}

/* Reverse of prot_to_pte for merging overlapping mappings */
static uint32_t pte_to_prot(uint64_t pte) {
    uint32_t p = UVM_PROT_READ;
    if (pte & VMM_WRITABLE) p |= UVM_PROT_WRITE;
    if (!(pte & VMM_NX)) p |= UVM_PROT_EXEC;
    return p;
}

static uint64_t page_down(uint64_t a) { return a & ~0xFFFULL; }
static uint64_t page_up(uint64_t a)   { return (a + 0xFFF) & ~0xFFFULL; }

/* Region bookkeeping: extend an overlapping region or add a new one. */
static void region_commit(process_t* proc, uint64_t start, uint64_t end, uint32_t prot) {
    for (int i = 0; i < MAX_VM_REGIONS; i++) {
        vm_region_t* r = &proc->regions[i];
        if (!r->used || r->end < start || r->start > end) continue;
        if (r->prot != prot) continue;
        if (start < r->start) r->start = start;
        if (end > r->end) r->end = end;
        return;
    }
    for (int i = 0; i < MAX_VM_REGIONS; i++) {
        vm_region_t* r = &proc->regions[i];
        if (!r->used) {
            r->used = 1;
            r->start = start;
            r->end = end;
            r->prot = prot;
            return;
        }
    }
    /* Table full: mappings stay in the page tables but become untracked.
     * Should never happen with MAX_VM_REGIONS=24 and our workloads. */
    klog("[uvm] region table full\n");
}

int uvm_map_range(process_t* proc, uint64_t start, uint64_t end, uint32_t prot) {
    (void)proc;
    if (end <= start) return -1;

    uint64_t s = page_down(start);
    uint64_t e = page_up(end);

    for (uint64_t v = s; v < e; v += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(v);
        if ((pte & VMM_PRESENT) && (pte & VMM_USER)) {
            /* Already a USER mapping (overlapping PT_LOADs): OR the
             * protections and keep the existing physical page + its
             * content. */
            uint32_t merged = pte_to_prot(pte) | prot;
            vmm_map_page(v, pte & ~0xFFF, prot_to_pte(merged));
        } else {
            /* Fresh anonymous page. NOTE: the kernel's identity map may
             * cover this VA with supervisor PTEs (present, U/S=0). Those
             * must NEVER be reused for user pages - they alias whatever
             * physical address equals the VA, which may not even exist
             * (writes silently vanish, reads return garbage). Always
             * allocate a fresh frame here. */
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                /* Roll back pages mapped so far: they are not yet
                 * tracked by any region, so uvm_release_all won't see them. */
                klog("[uvm] out of physical pages\n");
                for (uint64_t r = s; r < v; r += PAGE_SIZE) {
                    uint64_t pphys = vmm_get_phys(r);
                    if (pphys != 0) {
                        vmm_unmap_page(r);
                        pmm_free_page(pphys & ~0xFFF);
                    }
                }
                return -1;
            }
            vmm_map_page(v, phys, prot_to_pte(prot));
            memset((void*)v, 0, PAGE_SIZE);  /* anonymous pages are zeroed */
        }
    }

    region_commit(proc, s, e, prot);
    return 0;
}

int uvm_unmap_range(process_t* proc, uint64_t start, uint64_t end) {
    uint64_t s = page_down(start);
    uint64_t e = page_up(end);

    for (uint64_t v = s; v < e; v += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys(v);
        if (phys != 0) {
            vmm_unmap_page(v);
            pmm_free_page(phys & ~0xFFF);
        }
    }

    for (int i = 0; i < MAX_VM_REGIONS; i++) {
        vm_region_t* r = &proc->regions[i];
        if (!r->used || r->end <= s || r->start >= e) continue;
        if (r->start >= s && r->end <= e) {
            r->used = 0;                       /* fully covered: drop */
        } else if (r->start < s) {
            r->end = s;                        /* cut tail (or middle -> tail kept) */
        } else {
            r->start = e;                      /* cut head */
        }
    }
    return 0;
}

int uvm_change_prot(process_t* proc, uint64_t start, uint64_t end, uint32_t prot) {
    uint64_t s = page_down(start);
    uint64_t e = page_up(end);

    /* Every page must belong to a region before we touch page tables. */
    if (!uvm_check_range(proc, s, e - s)) return -1;

    for (uint64_t v = s; v < e; v += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys(v);
        if (phys == 0) continue;
        vmm_map_page(v, phys & ~0xFFF, prot_to_pte(prot));
    }

    for (int i = 0; i < MAX_VM_REGIONS; i++) {
        vm_region_t* r = &proc->regions[i];
        if (!r->used || r->end <= s || r->start >= e) continue;
        r->prot = prot;
    }
    return 0;
}

int uvm_check_range(process_t* proc, uint64_t uaddr, size_t len) {
    if (proc == NULL) return 0;
    if (len == 0) return 1;
    if (uaddr + len < uaddr) return 0;

    for (uint64_t a = page_down(uaddr); a < uaddr + len; a += PAGE_SIZE) {
        int inside = 0;
        for (int i = 0; i < MAX_VM_REGIONS; i++) {
            vm_region_t* r = &proc->regions[i];
            if (r->used && a >= r->start && a < r->end) {
                inside = 1;
                break;
            }
        }
        if (!inside) return 0;
        if (vmm_get_phys(a) == 0) return 0;
    }
    return 1;
}

uint64_t uvm_mmap_anon(process_t* proc, uint64_t hint, size_t len, uint32_t prot, int fixed) {
    size_t plen = (size_t)page_up(len);

    if (fixed) {
        uint64_t s = page_down(hint);
        if (s < UVM_ELF_BASE || s + plen > UVM_MMAP_LIMIT) return 0;
        uvm_unmap_range(proc, s, s + plen);
        if (uvm_map_range(proc, s, s + plen, prot) != 0) return 0;
        return s;
    }

    /* Bump allocation; freed VAs are not reused (musl malloc recycles
     * chunks in userland, so syscall-level churn is minimal). */
    if (proc->mmap_top < UVM_MMAP_BASE) proc->mmap_top = UVM_MMAP_BASE;
    uint64_t s = page_up(proc->mmap_top);
    if (s + plen > UVM_MMAP_LIMIT) return 0;
    if (uvm_map_range(proc, s, s + plen, prot) != 0) return 0;
    proc->mmap_top = s + plen;
    return s;
}

void uvm_release_all(process_t* proc) {
    for (int i = 0; i < MAX_VM_REGIONS; i++) {
        vm_region_t* r = &proc->regions[i];
        if (!r->used) continue;
        for (uint64_t v = r->start; v < r->end; v += PAGE_SIZE) {
            uint64_t phys = vmm_get_phys(v);
            if (phys != 0) {
                vmm_unmap_page(v);
                pmm_free_page(phys & ~0xFFF);
            }
        }
        r->used = 0;
    }
    proc->brk_start = 0;
    proc->brk_cur = 0;
    proc->mmap_top = 0;
}
