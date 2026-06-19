#include "stdlib.h"
#include "string.h"

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

void itoa(int num, char* str, int base) {
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
    
    while (n) {
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

void utoa(uint32_t num, char* str, int base) {
    char* ptr = str;
    char* low;
    uint32_t n = num;
    
    if (n == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }
    
    low = ptr;
    
    while (n) {
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