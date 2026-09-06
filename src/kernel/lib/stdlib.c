#include "lib/stdlib.h"
#include "lib/string.h"
#include <stdarg.h>

/* Phase 4: tiny bounded formatter for kernel-side string building.
 * Supports %s %d %u %x %c %% plus optional zero/space pad width for the
 * numeric forms (%02d, %04x, %08u, ...); always NUL-terminates. */
void kvsnprintf(char* buf, size_t size, const char* fmt, va_list ap_copy) {
    if (size == 0) return;
    size_t off = 0;
    va_list ap;
    va_copy(ap, ap_copy);

#define KSP_EMIT(c) do { if (off + 1 < size) buf[off++] = (char)(c); } while (0)

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            KSP_EMIT(*p);
            continue;
        }
        p++;
        if (*p == '%') {
            KSP_EMIT('%');
            continue;
        }
        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            if (s == NULL) s = "(null)";
            while (*s) KSP_EMIT(*s++);
            continue;
        }
        /* optional zero-pad flag + decimal width, e.g. %02x %08x %5d */
        const char* wstart = p;
        int zeropad = (*p == '0');
        if (zeropad) p++;
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        char spec = *p;
        if (spec == 'd' || spec == 'u' || spec == 'x') {
            char num[24];
            if (spec == 'd') itoa(va_arg(ap, int), num, 10, sizeof(num));
            else utoa(va_arg(ap, uint32_t), num, spec == 'x' ? 16 : 10, sizeof(num));
            char* q = num;
            if (*q == '-') {                    /* sign first, then pad */
                KSP_EMIT('-');
                q++;
            }
            int len = 0;
            for (char* r = q; *r; r++) len++;
            for (int i = len; i < width; i++) KSP_EMIT(zeropad ? '0' : ' ');
            while (*q) KSP_EMIT(*q++);
            continue;
        }
        if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            KSP_EMIT(c);
            continue;
        }
        /* unknown specifier: emit the raw text verbatim */
        KSP_EMIT('%');
        for (const char* q = wstart; q <= p; q++) KSP_EMIT(*q);
    }
    va_end(ap);
    buf[off] = '\0';
#undef KSP_EMIT
}

void ksprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
}

static uint32_t rand_seed = 1;

void srand(uint32_t seed) {
    rand_seed = seed;
}

uint32_t rand() {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

uint32_t strtoul(const char* str, char** endptr, int base) {
    uint32_t result = 0;
    
    while (*str >= '0' && *str <= '9') {
        result = result * base + (*str - '0');
        str++;
    }
    
    if (endptr) *endptr = (char*)str;
    return result;
}

void itoa(int num, char* str, int base, int max_size) {
    if (max_size < 2) { str[0] = '\0'; return; }
    char* ptr = str;
    char* low;
    int n = num;

    if (n == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }

    if (n < 0 && base == 10) {
        *ptr++ = '-';
        n = -n;
    }

    low = ptr;

    while (n && ptr - str < max_size - 1) {
        int remainder = n % base;
        *ptr++ = remainder < 10 ? remainder + '0' : remainder + 'A' - 10;
        n /= base;
    }

    *ptr-- = '\0';

    while (low < ptr) {
        char temp = *low;
        *low++ = *ptr;
        *ptr-- = temp;
    }
}

void utoa(uint32_t num, char* str, int base, int max_size) {
    if (max_size < 2) { str[0] = '\0'; return; }
    char* ptr = str;
    char* low;
    uint32_t n = num;
    
    if (n == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }
    
    low = ptr;

    while (n && ptr - str < max_size - 1) {
        int remainder = n % base;
        *ptr++ = remainder < 10 ? remainder + '0' : remainder + 'A' - 10;
        n /= base;
    }
    
    *ptr-- = '\0';
    
    while (low < ptr) {
        char temp = *low;
        *low++ = *ptr;
        *ptr-- = temp;
    }
}