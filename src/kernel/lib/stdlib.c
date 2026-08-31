#include "lib/stdlib.h"
#include "lib/string.h"
#include <stdarg.h>

/* Phase 4: tiny bounded formatter for kernel-side string building.
 * Supports %s %d %u %x %c %%; always NUL-terminates and truncates. */
void ksprintf(char* buf, size_t size, const char* fmt, ...) {
    if (size == 0) return;
    size_t off = 0;
    va_list ap;
    va_start(ap, fmt);

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            if (off + 1 < size) buf[off++] = *p;
            continue;
        }
        p++;
        if (*p == '%') {
            if (off + 1 < size) buf[off++] = '%';
            continue;
        }
        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            if (s == NULL) s = "(null)";
            while (*s && off + 1 < size) buf[off++] = *s++;
            continue;
        }
        if (*p == 'd' || *p == 'u' || *p == 'x') {
            char num[24];
            if (*p == 'd') itoa(va_arg(ap, int), num, 10, sizeof(num));
            else utoa(va_arg(ap, uint32_t), num, *p == 'x' ? 16 : 10, sizeof(num));
            for (char* q = num; *q && off + 1 < size; q++) buf[off++] = *q;
            continue;
        }
        if (*p == 'c') {
            char c = (char)va_arg(ap, int);
            if (off + 1 < size) buf[off++] = c;
            continue;
        }
        /* unknown specifier: emit literally */
        if (off + 1 < size) buf[off++] = '%';
        if (*p && off + 1 < size) buf[off++] = *p;
    }
    va_end(ap);
    buf[off] = '\0';
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