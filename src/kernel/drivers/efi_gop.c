/* UEFI Graphics Output Protocol: the kernel itself locates GOP, selects a
 * 32bpp mode, snapshots the EFI memory map and calls ExitBootServices -
 * all while GRUB (multiboot2 EFI_BS header tag) keeps boot services alive.
 *
 * Runtime context: called first thing in kernel_main with IF=0, no heap
 * and no display stack - logging goes straight to COM1.  All EFI calls
 * use the Microsoft x64 calling convention, marshalled by the
 * efi_call2..6 thunks below.  Structure offsets are the EDK2 x64 layouts
 * (EFI_SYSTEM_TABLE.BootServices=0x60; EFI_BOOT_SERVICES: GetMemoryMap
 * =0x38, ExitBootServices=0xE8, LocateProtocol=0x140; GOP: QueryMode=0x00,
 * SetMode=0x08, Mode=0x18; MODE: Info=0x08, FrameBufferBase=0x18,
 * FrameBufferSize=0x20).
 */
#include "drivers/efi_gop.h"
#include "drivers/fb.h"
#include "drivers/io.h"
#include "lib/string.h"

/* --- multiboot2 boot-info tags ----------------------------------------- */
#define MB2_TAG_EFI64_ST  12    /* u32 type,u32 size,u64 system_table ptr */
#define MB2_TAG_EFI_BS    18    /* u32 type,u32 size - boot services alive */
#define MB2_TAG_EFI64_IH  20    /* u32 type,u32 size,u64 image handle */

/* --- EFI status codes we branch on -------------------------------------- */
#define EFI_SUCCESS              0ULL
#define EFI_INVALID_PARAMETER    0x8000000000000002ULL
#define EFI_BUFFER_TOO_SMALL     0x8000000000000005ULL

/* --- Microsoft x64 ABI thunks (SysV args -> EFI call) -------------------
 * efi_callN(fn, a1..aN) invokes the EFI function at fn with a1..aN.
 * Boot services functions take plain args (no This); GOP protocol
 * member functions take This as their first argument, passed explicitly
 * by the caller.  SysV entry: rdi=fn, rsi=a1, rdx=a2, rcx=a3, r8=a4,
 * r9=a5, [rsp+8]=a6.  MS x64 call: rcx=a1, rdx=a2, r8=a3, r9=a4,
 * [rsp+40]=a5 (callee view), 32B shadow space.  Entering each thunk
 * rsp = 8 (mod 16); each sequence returns rsp to 0 (mod 16) at the
 * call instruction. */
extern uint64_t efi_call2(void* fn, uint64_t a1, uint64_t a2);
extern uint64_t efi_call3(void* fn, uint64_t a1, uint64_t a2, uint64_t a3);
extern uint64_t efi_call4(void* fn, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4);
extern uint64_t efi_call5(void* fn, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5);

__asm__(
".text\n"
"efi_call2:\n\t"
    "mov %rsi,%rcx\n\t" "mov %rdi,%rax\n\t"
    "sub $40,%rsp\n\t" "call *%rax\n\t" "add $40,%rsp\n\t" "ret\n"
"efi_call3:\n\t"
    "mov %rcx,%r8\n\t" "mov %rsi,%rcx\n\t"
    "mov %rdi,%rax\n\t"
    "sub $40,%rsp\n\t" "call *%rax\n\t" "add $40,%rsp\n\t" "ret\n"
"efi_call4:\n\t"
    "mov %r8,%r9\n\t" "mov %rcx,%r8\n\t" "mov %rsi,%rcx\n\t"
    "mov %rdi,%rax\n\t"
    "sub $40,%rsp\n\t" "call *%rax\n\t" "add $40,%rsp\n\t" "ret\n"
"efi_call5:\n\t"
    "mov %r9,%r10\n\t" "mov %r8,%r11\n\t"
    "mov %rcx,%r8\n\t" "mov %rsi,%rcx\n\t" "mov %r11,%r9\n\t"
    "mov %rdi,%rax\n\t"
    "sub $56,%rsp\n\t"
    "mov %r10,32(%rsp)\n\t"   /* stack arg5 */
    "call *%rax\n\t" "add $56,%rsp\n\t" "ret\n"
".pushsection .note.GNU-stack,\"\",@progbits\n"
".popsection\n"
);

/* --- stray-interrupt shield ----------------------------------------------
 * The firmware IDT is still active here, and its gates reference the
 * firmware's code segment (0x38), which does not exist in the kernel GDT.
 * EDK2 re-asserts sti inside every boot-services call when it restores TPL,
 * and something in the firmware path re-opens the PIT's IRQ0 on the 8259
 * (boot.asm already masked both PICs at entry): the next tick then vectors
 * through the firmware IDT under the kernel GDT and triple-faults as
 * #GP(sel 0x38) -> #DF -> triple.  Fix: install a flat 256-gate stub IDT
 * before the first boot-services call so stray vectors are swallowed, and
 * re-mask both PICs.  The kernel's own interrupt init lidts over this. */

extern uint8_t efi_stub_plain[];
extern uint8_t efi_stub_ec[];

__asm__(
".text\n"
".globl efi_stub_plain\n"
".globl efi_stub_ec\n"
"efi_stub_plain:\n\t"
    "push %rax\n\t"
    "push %rcx\n\t"
    "mov $0x20,%al\n\t"
    "out %al,$0x20\n\t"        /* EOI master (spurious-safe) */
    "out %al,$0xA0\n\t"        /* EOI slave  */
    "movabs $0xfee000b0,%rcx\n\t"
    "xor %eax,%eax\n\t"
    "mov %eax,(%rcx)\n\t"      /* LAPIC EOI (delivery may route via LAPIC) */
    "pop %rcx\n\t"
    "pop %rax\n\t"
    "iretq\n"
"efi_stub_ec:\n\t"             /* error-code exceptions: drop the code */
    "add $8,%rsp\n\t"
    "jmp efi_stub_plain\n"
".pushsection .note.GNU-stack,\"\",@progbits\n"
".popsection\n"
);

static uint64_t efi_idt[256 * 2] __attribute__((aligned(16)));
static struct { uint16_t limit; uint64_t base; } __attribute__((packed))
    efi_idtr;

static void efi_install_stub_idt(void) {
    static const uint8_t ec_vecs[] = { 8, 10, 11, 12, 13, 14, 17, 21 };
    for (unsigned v = 0; v < 256; v++) {
        uint64_t fn = (uint64_t)efi_stub_plain;
        for (unsigned k = 0; k < sizeof(ec_vecs) / sizeof(ec_vecs[0]); k++)
            if (ec_vecs[k] == v) { fn = (uint64_t)efi_stub_ec; break; }
        uint64_t* g = &efi_idt[v * 2];
        g[0] = (fn & 0xFFFFULL)
             | (0x08ULL << 16)              /* selector: kernel code */
             | (0x8EULL << 40)              /* P | DPL0 | 64-bit int gate */
             | (((fn >> 16) & 0xFFFF) << 48);
        g[1] = fn >> 32;
    }
    efi_idtr.limit = 256 * 16 - 1;
    efi_idtr.base  = (uint64_t)efi_idt;
    __asm__ volatile("lidt %0" :: "m"(efi_idtr));
    outb(0x21, 0xFF);                       /* re-mask both 8259s */
    outb(0xA1, 0xFF);
}

/* --- early COM1 logging (klog needs pit + vga, none up yet) ------------- */

static void efi_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

static void efi_log(const char* s) {
    while (*s) {
        if (*s == '\n') efi_putc('\r');
        efi_putc(*s++);
    }
}

static void efi_log_hex(const char* prefix, uint64_t v) {
    char buf[19];
    static const char hex[] = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    efi_log(prefix);
    efi_log(buf);
    efi_log("\n");
}

/* --- static state (no heap: bss only) ----------------------------------- */

/* GOP GUID 9042a9de-23dc-4a38-96fb-7aded080516a, little-endian memory image:
 * Data1=0x9042a9de -> de a9 42 90; Data2=0x23dc -> dc 23; Data3=0x4a38
 * -> 38 4a; then Data4 verbatim.  (The 23dc/4a38 halves are u16 fields -
 * writing the string pairs verbatim here was exactly why every
 * ByProtocol search returned EFI_NOT_FOUND.) */
static const uint8_t gop_guid[16] = {
    0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
    0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a
};

/* EFI_MEMORY_DESCRIPTOR layout (x64): u32 type, u32 pad, u64 phys_start,
 * u64 virt_start, u64 pages, u64 attribute; desc size given by firmware */
#define EFI_MEMORY_TYPE_CONVENTIONAL 7

static uint8_t  efi_mmap_buf[65536];
static uint32_t efi_mmap_size_;
static uint32_t efi_mmap_descsz;
static uint32_t efi_mmap_descver;
static int      efi_mmap_ok;

static void* mb2_find_tag(uint64_t mbi_phys, uint32_t type) {
    if (mbi_phys == 0) return NULL;
    uint8_t* info = (uint8_t*)mbi_phys;
    uint32_t total = *(uint32_t*)info;
    uint8_t* tag = info + 8;
    while (tag + 8 <= info + total) {
        uint32_t ttype = *(uint32_t*)(tag + 0);
        uint32_t tsize = *(uint32_t*)(tag + 4);
        if (ttype == 0) break;                    /* end tag */
        if (ttype == type && tsize >= 8) return tag;
        uint32_t advance = (tsize + 7) & ~7;
        if (advance == 0) break;                  /* malformed */
        tag += advance;
    }
    return NULL;
}

static void efi_snapshot_mmap(uint64_t bs, uint64_t* map_key_out) {
    uint64_t sz    = sizeof(efi_mmap_buf);
    uint64_t key   = 0;
    uint64_t dsz   = 0;
    uint64_t dver  = 0;
    /* GetMemoryMap(&Size, Map, &MapKey, &DescriptorSize, &Version)
     * (boot services functions carry no This argument) */
    uint64_t rc = efi_call5(*(void**)(bs + 0x38),
                            (uint64_t)&sz, (uint64_t)efi_mmap_buf,
                            (uint64_t)&key, (uint64_t)&dsz, (uint64_t)&dver);
    if (rc != EFI_SUCCESS) {
        efi_log_hex("[gop] GetMemoryMap failed rc=", rc);
        return;
    }
    efi_mmap_size_  = (uint32_t)sz;
    efi_mmap_descsz = (uint32_t)dsz;
    efi_mmap_descver= (uint32_t)dver;
    efi_mmap_ok     = 1;
    *map_key_out    = key;
}

/* --- public snapshot accessors ------------------------------------------ */

int      efi_memory_map_available(void)      { return efi_mmap_ok; }
const uint8_t* efi_memory_map_ptr(void)      { return efi_mmap_buf; }
uint32_t efi_memory_map_size(void)           { return efi_mmap_size_; }
uint32_t efi_memory_map_desc_size(void)      { return efi_mmap_descsz; }
uint32_t efi_memory_map_desc_version(void)   { return efi_mmap_descver; }

/* framebuffer reservation info for the PMM (0 when inactive) */
static uint64_t fb_base_;
static uint64_t fb_size_;

uint64_t fb_base(void) { return fb_base_; }
uint64_t fb_size(void) { return fb_size_; }

/* --- main entry ---------------------------------------------------------- */

void efi_gop_init(uint64_t mb) {
    /* tag 12 (EFI64 system table) + tag 18 (boot services kept alive) are
     * both required; BIOS boots simply never carry them */
    uint8_t* st_tag = mb2_find_tag(mb, MB2_TAG_EFI64_ST);
    uint8_t* bs_tag = mb2_find_tag(mb, MB2_TAG_EFI_BS);
    if (st_tag == NULL || bs_tag == NULL) {
        efi_log("[gop] no EFI ST (BIOS boot)\n");
        return;
    }

    /* shield must be up before ANY boot-services call - see block comment */
    efi_install_stub_idt();
    efi_log_hex("[gop] stub idt, pic imr=", inb(0x21));

    uint64_t st = *(uint64_t*)(st_tag + 8);
    if (st == 0 || st + 0x70 >= 0x100000000ULL) {
        efi_log_hex("[gop] system table out of range ", st);
        return;
    }
    efi_log_hex("[gop] st=", st);
    efi_log_hex("[gop] st sig=", *(uint64_t*)st);
    uint64_t bs = *(uint64_t*)(st + 0x60);
    if (bs == 0) {
        efi_log("[gop] null boot services\n");
        return;
    }
    efi_log_hex("[gop] bs=", bs);
    efi_log_hex("[gop] bs sig=", *(uint64_t*)bs);
    efi_log_hex("[gop] locate fn=", (uint64_t)(*(void**)(bs + 0x140)));

    /* probe 1: GetMemoryMap size-query must return BUFFER_TOO_SMALL when
     * boot services are really alive (post-EBS they are gone) */
    {
        uint64_t sz = 0, key = 0, dsz = 0, dver = 0;
        uint64_t rc1 = efi_call5(*(void**)(bs + 0x38), (uint64_t)&sz, 0,
                                 (uint64_t)&key, (uint64_t)&dsz,
                                 (uint64_t)&dver);
        efi_log_hex("[gop] mmap probe rc=", rc1);
        efi_log_hex("[gop] mmap need sz=", sz);
    }

    /* probe 2: LocateHandleBuffer(ByProtocol, GOP, NULL, &n, &buf) */
    {
        uint64_t n = 0, buf = 0;
        uint64_t rc2 = efi_call5(*(void**)(bs + 0x138),
                                 2 /*ByProtocol*/, (uint64_t)gop_guid, 0,
                                 (uint64_t)&n, (uint64_t)&buf);
        efi_log_hex("[gop] lhb rc=", rc2);
        efi_log_hex("[gop] lhb count=", n);
    }

    /* probe 3: HandleProtocol on the console-out handle (SimpleTextOut).
     * GUID 38747777-c5fc-4c21-a163-2f8b3fe11975, little-endian memory image:
     * Data1=0x38747777 -> 77 77 74 38; Data2=0xc5fc -> fc c5; Data3=0x4c21
     * -> 21 4c; then Data4 verbatim. */
    {
        static const uint8_t sto_guid[16] = {
            0x77, 0x77, 0x74, 0x38, 0xfc, 0xc5, 0x21, 0x4c,
            0xa1, 0x63, 0x2f, 0x8b, 0x3f, 0xe1, 0x19, 0x75
        };
        uint64_t ih2 = 0;
        uint64_t out_h = *(uint64_t*)(st + 0x38);   /* ConsoleOutHandle */
        efi_log_hex("[gop] conout handle=", out_h);
        uint64_t rc3 = efi_call3(*(void**)(bs + 0x98), out_h,
                                 (uint64_t)sto_guid, (uint64_t)&ih2);
        efi_log_hex("[gop] handleproto sto rc=", rc3);
        efi_log_hex("[gop] sto iface=", ih2);
    }

    /* probe 3b: LocateHandleBuffer(AllHandles) - is the handle database
     * itself populated at kernel time? */
    {
        uint64_t n = 0, buf = 0;
        uint64_t rc4 = efi_call5(*(void**)(bs + 0x138),
                                 0 /*AllHandles*/, 0, 0,
                                 (uint64_t)&n, (uint64_t)&buf);
        efi_log_hex("[gop] lhb all rc=", rc4);
        efi_log_hex("[gop] lhb all n=", n);
    }

    /* probe 3c: LocateHandle (0xB0) ByProtocol GOP size-query - the exact
     * API GRUB's efi_gop videoinfo uses.  BUFFER_TOO_SMALL => handles match;
     * NOT_FOUND => the database genuinely has no GOP right now. */
    {
        static const uint8_t sto2[16] = {
            0x77, 0x77, 0x74, 0x38, 0xfc, 0xc5, 0x21, 0x4c,
            0xa1, 0x63, 0x2f, 0x8b, 0x3f, 0xe1, 0x19, 0x75
        };
        uint64_t sz = 0;
        uint64_t rc5 = efi_call5(*(void**)(bs + 0xB0),
                                 2 /*ByProtocol*/, (uint64_t)gop_guid, 0,
                                 (uint64_t)&sz, 0 /*NULL buffer*/);
        efi_log_hex("[gop] lh gop rc=", rc5);
        efi_log_hex("[gop] lh gop need=", sz);
        sz = 0;
        uint64_t rc6 = efi_call5(*(void**)(bs + 0xB0),
                                 2 /*ByProtocol*/, (uint64_t)sto2, 0,
                                 (uint64_t)&sz, 0);
        efi_log_hex("[gop] lh sto rc=", rc6);
        efi_log_hex("[gop] lh sto need=", sz);
    }

    /* probe 3d: runtime-services signature sanity ("RUNTSERV") */
    {
        uint64_t rt = *(uint64_t*)(st + 0x58);
        efi_log_hex("[gop] rt=", rt);
        if (rt) efi_log_hex("[gop] rt sig=", *(uint64_t*)rt);
    }

    /* probe 3e: byte-dump the first 24 bytes of key boot-services function
     * bodies.  Real DxeCore code has standard prologues; a table rebuilt
     * with stubs would show e.g. "b8 xx 00 00 00 c3" (mov eax,imm; ret).
     * Also self-check that our own gop_guid bytes are where we think. */
    {
        static const struct { const char* name; uint32_t off; } fns[] = {
            { "getmemmap", 0x38 }, { "handleproto", 0x98 },
            { "locatehnd", 0xB0 }, { "lhb", 0x138 },
            { "locateproto", 0x140 }, { "ebs", 0xE8 },
        };
        for (unsigned k = 0; k < sizeof(fns) / sizeof(fns[0]); k++) {
            uint64_t fn = *(uint64_t*)(bs + fns[k].off);
            efi_log_hex("[gop] fn ", fns[k].off);
            efi_log_hex("      ptr=", fn);
            if (fn >= 0x1000 && fn + 24 < 0x100000000ULL) {
                uint8_t* p = (uint8_t*)fn;
                for (unsigned i = 0; i < 24; i += 4) {
                    efi_log_hex("      b=", ((uint64_t)p[i]) |
                                             ((uint64_t)p[i+1] << 8) |
                                             ((uint64_t)p[i+2] << 16) |
                                             ((uint64_t)p[i+3] << 24));
                }
            }
        }
        efi_log_hex("[gop] guid self=", *(uint32_t*)gop_guid);
    }

    /* LocateProtocol(Protocol, Registration, Interface) - the table slot
     * holds a function POINTER, dereference it before calling */
    uint64_t gop = 0;
    uint64_t rc = efi_call3(*(void**)(bs + 0x140),
                            (uint64_t)gop_guid, 0, (uint64_t)&gop);
    if (rc != EFI_SUCCESS || gop == 0) {
        efi_log_hex("[gop] gop_fail reason=locate rc=", rc);
        return;
    }

    /* enumerate modes: first pass wants exactly 1024x768x32, second pass
     * accepts any 32bpp linear mode */
    uint64_t mode_ptr = *(uint64_t*)(gop + 0x18);
    if (mode_ptr == 0) {
        efi_log("[gop] gop_fail reason=modeptr\n");
        return;
    }
    uint32_t max_mode = *(uint32_t*)(mode_ptr + 0x00);

    int chosen = -1;
    for (int pass = 0; pass < 2 && chosen < 0; pass++) {
        for (uint32_t i = 0; i < max_mode; i++) {
            uint64_t sz = 0, info = 0;
            /* QueryMode(This, ModeNumber, &SizeOfInfo, &Info) */
            rc = efi_call4(*(void**)gop, gop, i, (uint64_t)&sz,
                           (uint64_t)&info);
            if (rc != EFI_SUCCESS || info == 0 || sz < 36) continue;
            uint32_t w  = *(uint32_t*)(info + 4);
            uint32_t h  = *(uint32_t*)(info + 8);
            uint32_t pf = *(uint32_t*)(info + 12);
            if (pf > 1) continue;                 /* want RGBX/BGRX only */
            int ok;
            if (pass == 0) ok = (w == 1024 && h == 768);
            else           ok = 1;
            if (ok) {
                chosen = (int)i;
                break;
            }
        }
    }
    if (chosen < 0) {
        efi_log("[gop] gop_fail reason=nomode\n");
        return;
    }

    /* SetMode(This, ModeNumber) - the slot at gop+0x08 HOLDS the function
     * pointer, so it must be dereferenced.  Passing gop+0x08 itself made
     * the CPU execute the GOP struct bytes as code (the #UD at the
     * interface address seen during bring-up). */
    rc = efi_call2(*(void**)(gop + 0x08), gop, (uint64_t)chosen);
    if (rc != EFI_SUCCESS) {
        efi_log_hex("[gop] gop_fail reason=setmode rc=", rc);
        return;
    }

    /* re-read the active mode: SetMode may refresh Mode->Info */
    mode_ptr = *(uint64_t*)(gop + 0x18);
    uint64_t info = *(uint64_t*)(mode_ptr + 0x08);
    uint64_t fbb  = *(uint64_t*)(mode_ptr + 0x18);
    uint64_t fbs  = *(uint64_t*)(mode_ptr + 0x20);
    if (info == 0 || fbb == 0) {
        efi_log("[gop] gop_fail reason=info\n");
        return;
    }
    uint32_t w  = *(uint32_t*)(info + 4);
    uint32_t h  = *(uint32_t*)(info + 8);
    uint32_t pf = *(uint32_t*)(info + 12);
    uint32_t pp = *(uint32_t*)(info + 32);
    uint32_t pitch = pp * 4;

    /* the boot page tables identity-map only the first 4 GiB */
    if (fbb + fbs > 0x100000000ULL) {
        efi_log_hex("[gop] gop_fail reason=fbaddr fb=", fbb);
        return;
    }

    fb_set_info(fbb, pitch, w, h, pf == 0 /* rgbx */);
    fb_base_ = fbb;
    fb_size_ = fbs;

    char msg[96];
    const char* pf_name = (pf == 0) ? "RGBX" : "BGRX";
    /* mini formatter: "[gop] gop_ok mode=WxHx32 fmt fb= pitch=" */
    {
        char* p = msg;
        const char* s = "[gop] gop_ok mode=";
        while (*s) *p++ = *s++;
        /* width */
        uint32_t v = w; char tmp[12]; int n = 0;
        do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
        while (n) *p++ = tmp[--n];
        *p++ = 'x';
        v = h; n = 0;
        do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
        while (n) *p++ = tmp[--n];
        *p++ = 'x'; *p++ = '3'; *p++ = '2'; *p++ = ' ';
        s = pf_name; while (*s) *p++ = *s++;
        s = " fb=";
        while (*s) *p++ = *s++;
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) *p++ = hex[(fbb >> (60 - i * 4)) & 0xF];
        s = " pitch=";
        while (*s) *p++ = *s++;
        v = pitch; n = 0;
        do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
        while (n) *p++ = tmp[--n];
        *p++ = '\n'; *p = '\0';
    }
    efi_log(msg);

    /* snapshot the EFI memory map BEFORE ExitBootServices (GRUB keep_bs
     * boots carry no multiboot2 mmap tag, so PMM feeds off this snapshot) */
    uint64_t map_key = 0;
    efi_snapshot_mmap(bs, &map_key);
    efi_log_hex("[gop] pre-ebs pic imr=", inb(0x21));

    /* ExitBootServices(ImageHandle, MapKey).  EDK2 ignores ImageHandle,
     * but pass the real one when the loader provided it (tag 20). */
    uint64_t ih = 0;
    uint8_t* ih_tag = mb2_find_tag(mb, MB2_TAG_EFI64_IH);
    if (ih_tag != NULL) ih = *(uint64_t*)(ih_tag + 8);

    int ebs_done = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = efi_call2(*(void**)(bs + 0xE8), ih, map_key);
        if (rc == EFI_SUCCESS) { ebs_done = 1; break; }
        /* stale MapKey: re-snapshot and retry */
        map_key = 0;
        efi_snapshot_mmap(bs, &map_key);
        if (rc == EFI_INVALID_PARAMETER && efi_mmap_ok) continue;
        break;
    }
    if (ebs_done) {
        efi_log("[gop] ebs_ok\n");
    } else {
        efi_log_hex("[gop] ebs_fail rc=", rc);
    }

    fb_clear();
}
