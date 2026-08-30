#ifndef UVM_H
#define UVM_H

#include "lib/types.h"
#include "core/process.h"

/* User-space protection bits (mirrors Linux PROT_*) */
#define UVM_PROT_READ  0x1
#define UVM_PROT_WRITE 0x2
#define UVM_PROT_EXEC  0x4

/* User VM layout. The kernel heap arena is identity-mapped at
 * 0x200000..0x10000000 (see USER_CODE_BASE comment in process.h), so user
 * VAs must start above it. Single user process at a time shares the kernel
 * page tables, which is why these are fixed global ranges. */
#define UVM_ELF_BASE    0x10000000ULL  /* lowest legal user VA (256 MB) */
#define UVM_BRK_LIMIT   0x17000000ULL  /* brk heap may grow up to here */
#define UVM_MMAP_BASE   0x18000000ULL  /* anonymous mmap arena start */
#define UVM_MMAP_LIMIT  0x7F000000ULL  /* below the user stack */

/* Map [start, end) (rounded out to page boundaries) into the shared page
 * tables and record a region. Fresh pages are zeroed; pages already mapped
 * (overlapping PT_LOADs) keep their physical page and get the OR of both
 * protections. Returns 0 or -1. */
int uvm_map_range(process_t* proc, uint64_t start, uint64_t end, uint32_t prot);

/* Unmap every page in [start, end) and drop/shrink overlapping regions.
 * Pages outside any mapping are ignored. Returns 0. */
int uvm_unmap_range(process_t* proc, uint64_t start, uint64_t end);

/* Change protection of the mapped pages inside [start, end). The range
 * must lie inside mapped regions; returns 0 or -1 (ENOMEM). */
int uvm_change_prot(process_t* proc, uint64_t start, uint64_t end, uint32_t prot);

/* True if [uaddr, uaddr+len) is inside some region of the process and
 * every touched page is present in the page tables. */
int uvm_check_range(process_t* proc, uint64_t uaddr, size_t len);

/* Anonymous mmap: MAP_FIXED honors hint (unmapping overlaps first),
 * otherwise a bump allocation from the mmap arena is returned.
 * Returns the user VA or 0 on failure. */
uint64_t uvm_mmap_anon(process_t* proc, uint64_t hint, size_t len,
                       uint32_t prot, int fixed);

/* Release every region of the process (unmap + free physical pages). */
void uvm_release_all(process_t* proc);

/* fork: copy all mapped user pages of parent into fresh frames of child
 * and mirror the region table + brk/mmap cursors. Returns 0 or -1. */
int uvm_fork(process_t* parent, process_t* child);

/* Copy into/out of a user VA of the given process through the identity
 * map (works regardless of which CR3 is active). */
void uvm_write_user_va(process_t* proc, uint64_t dst_va, const void* src,
                       size_t len);
void uvm_read_user_va(process_t* proc, void* dst, uint64_t src_va, size_t len);

#endif
