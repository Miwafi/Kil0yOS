/*
 * linux/types.h - Linux kernel type compatibility for Kil0yOS
 *
 * Maps the basic integer types used by Linux NIC drivers onto the
 * project's freestanding types. Little-endian (x86-64) accessors are
 * no-ops. Also provides the doubly linked list used by most drivers.
 */
#ifndef _COMPAT_LINUX_TYPES_H
#define _COMPAT_LINUX_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u64 __be64;

typedef __u16 __sum16;
typedef __u32 __wsum;

typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
typedef uint16_t __bitwise;

typedef unsigned int  uint;
typedef unsigned char uchar;

/* Un-underscored kernel integer types (drivers use both spellings) */
typedef __u8  u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s8  s8;
typedef __s16 s16;
typedef __s32 s32;
typedef __s64 s64;

/* u64 statistics sync: uniprocessor, so the seqcounter is vestigial */
struct u64_stats_sync { unsigned int seq; };
static inline void u64_stats_init(struct u64_stats_sync* s)            { s->seq = 0; }
static inline void u64_stats_update_begin(struct u64_stats_sync* s)    { (void)s; }
static inline void u64_stats_update_end(struct u64_stats_sync* s)      { (void)s; }
static inline unsigned int u64_stats_fetch_begin(const struct u64_stats_sync* s) {
    (void)s; return 0;
}
static inline unsigned int u64_stats_fetch_retry(const struct u64_stats_sync* s,
                                                 unsigned int start) {
    (void)s; (void)start; return 0;
}

/* Little-endian host (x86): byte order conversions are no-ops */
static inline __le16  cpu_to_le16(__u16 v) { return v; }
static inline __le32  cpu_to_le32(__u32 v) { return v; }
static inline __le64  cpu_to_le64(__u64 v) { return v; }
static inline __u16   le16_to_cpu(__le16 v) { return v; }
static inline __u32   le32_to_cpu(__le32 v) { return v; }
static inline __u64   le64_to_cpu(__le64 v) { return v; }
static inline __be16  cpu_to_be16(__u16 v) { return (__u16)((v << 8) | (v >> 8)); }
static inline __be32  cpu_to_be32(__u32 v) {
    return ((__u32)(v >> 24)) | ((__u32)((v >> 8) & 0xFF) << 8) |
           ((__u32)((v << 8) & 0xFF00) << 8) | ((__u32)(v << 24));
}
static inline __be32  cpu_to_beip(__u32 v) { return cpu_to_be32(v); }
static inline __u32   be32_to_cpu(__be32 v) { return cpu_to_be32(v); }

/* ---- doubly linked list (subset of linux/list.h, good enough for NICs) ---- */
struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list->prev = list;
}
static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}
static inline void __list_add(struct list_head *newh,
                              struct list_head *prev,
                              struct list_head *next) {
    next->prev = newh;
    newh->next = next;
    newh->prev = prev;
    prev->next = newh;
}
static inline void list_add(struct list_head *newh, struct list_head *head) {
    __list_add(newh, head, head->next);
}
static inline void list_add_tail(struct list_head *newh, struct list_head *head) {
    __list_add(newh, head->prev, head);
}
static inline void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry->prev = entry;
}
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)
#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_for_each_entry(pos, head, member)                          \
    for (pos = list_first_entry(head, __typeof__(*pos), member);            \
         &pos->member != (head);                                        \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member)                  \
    for (pos = list_first_entry(head, __typeof__(*pos), member),            \
         n = list_next_entry(pos, member);                              \
         &pos->member != (head);                                        \
         pos = n, n = list_next_entry(n, member))

#endif /* _COMPAT_LINUX_TYPES_H */
