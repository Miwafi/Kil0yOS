#ifndef ELF_H
#define ELF_H

#include "lib/types.h"
#include "core/process.h"

/* ELF64 loader (Phase 0.1): parses the ELF header and program headers,
 * maps every PT_LOAD at its linked virtual address, zero-fills BSS
 * (p_memsz > p_filesz) and reports the entry point.
 *
 * Layout rule: the kernel heap is identity-mapped below 0x10000000, so
 *  - ET_EXEC images must already be linked at >= UVM_ELF_BASE
 *    (e.g. musl-gcc -static -Wl,-Ttext-segment=0x10000000)
 *  - ET_DYN images are placed with their lowest segment at UVM_ELF_BASE;
 *    static-PIE self-relocates, dynamic-PIE is relocated by the
 *    interpreter (ld-musl), which elf_load reports via interp[].
 *  - The interpreter itself is mapped by elf_load_interp at an explicit
 *    base (UVM_INTERP_BASE). */

#define ELF_INTERP_MAX 64

typedef struct {
    uint64_t entry;      /* user entry point (bias applied) */
    uint64_t brk_start;  /* page-aligned end of the image (heap base) */
    uint64_t phdr;       /* user VA of the program header array */
    uint16_t phnum;
    char interp[ELF_INTERP_MAX];  /* PT_INTERP path, "" if none */
} elf_load_result_t;

/* Quick magic/class check for format sniffing on the exec path */
int elf_is_elf64(const uint8_t* data, size_t size);

/* Load an ELF64 image into proc. PT_INTERP is parsed into out->interp;
 * mapping the interpreter is the caller's job. Returns 0 or a negative
 * ELF_ERR_*. */
#define ELF_ERR_FORMAT    (-1)   /* not a valid ELF64 x86-64 image */
#define ELF_ERR_LAYOUT    (-3)   /* ET_EXEC linked below UVM_ELF_BASE */
#define ELF_ERR_MAP       (-4)   /* out of memory while mapping */
#define ELF_ERR_PHDRS     (-5)   /* program headers not inside a PT_LOAD */

int elf_load(process_t* proc, const uint8_t* data, size_t size,
             elf_load_result_t* out);

/* Load an ET_DYN image (the dynamic interpreter) at an explicit base.
 * out->brk_start is the page-aligned end of the mapped image. */
int elf_load_interp(process_t* proc, const uint8_t* data, size_t size,
                    uint64_t base, elf_load_result_t* out);

#endif
