#include "string.h"

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

int strcmp(const char* str1, const char* str2) {
    while (*str1 && *str2 && *str1 == *str2) {
        str1++;
        str2++;
    }
    return *str1 - *str2;
}

char* strchr(const char* str, int c) {
    while (*str && *str != c) str++;
    return (*str == c) ? (char*)str : NULL;
}

static char* strtok_state = NULL;

char* strtok(char* str, const char* delim) {
    if (str == NULL) str = strtok_state;
    if (str == NULL) return NULL;
    
    while (*str) {
        const char* d = delim;
        int match = 0;
        while (*d) {
            if (*str == *d) {
                match = 1;
                break;
            }
            d++;
        }
        if (!match) break;
        str++;
    }
    
    if (*str == '\0') return NULL;
    
    char* start = str;
    while (*str) {
        const char* d = delim;
        while (*d) {
            if (*str == *d) {
                *str = '\0';
                strtok_state = str + 1;
                return start;
            }
            d++;
        }
        str++;
    }
    
    strtok_state = NULL;
    return start;
}

void* memset(void* ptr, int value, size_t num) {
    uint8_t* p = (uint8_t*)ptr;
    while (num--) *p++ = value;
    return ptr;
}

void* memcpy(void* dest, const void* src, size_t num) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (num--) *d++ = *s++;
    return dest;
}