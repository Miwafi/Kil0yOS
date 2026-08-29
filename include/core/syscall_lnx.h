#ifndef SYSCALL_LNX_H
#define SYSCALL_LNX_H

#include "lib/types.h"

/* Linux x86-64 ABI syscall layer (Phase 0).
 *
 * Entry: `syscall` instruction, number in RAX, args in RDI/RSI/RDX/R10/R8/R9,
 * return in RAX (negative = -errno). Handled by syscall_lnx_entry in isr.asm
 * which switches to the process kernel stack and calls the dispatcher below.
 * The legacy int 0x80 table (syscall.h) keeps serving KIL0 raw programs. */

/* Initialize MSRs (EFER.SCE, STAR, LSTAR, FMASK) and the dispatch table */
void syscall_lnx_init(void);

/* asm entry point (isr.asm), loaded into LSTAR */
void syscall_lnx_entry(void);

/* C dispatcher called from syscall_lnx_entry */
uint64_t syscall_lnx_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5);

/* arch_prctl support shared with process.c */
#define MSR_FS_BASE 0xC0000100
void syscall_lnx_write_fs_base(uint64_t base);

#endif
