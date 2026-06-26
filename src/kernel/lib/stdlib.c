#include "lib/stdlib.h"
#include "lib/string.h"

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