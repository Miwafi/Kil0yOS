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

int strncmp(const char* str1, const char* str2, size_t num) {
    while (num && *str1 && *str2 && *str1 == *str2) {
        str1++;
        str2++;
        num--;
    }
    if (num == 0) return 0;
    return *str1 - *str2;
}

char* strncpy(char* dest, const char* src, size_t num) {
    char* d = dest;
    while (num && *src) {
        *d++ = *src++;
        num--;
    }
    while (num--) {
        *d++ = '\0';
    }
    return dest;
}

char* strrchr(const char* str, int c) {
    char* last = NULL;
    while (*str) {
        if (*str == c) last = (char*)str;
        str++;
    }
    return last;
}

void* memmove(void* dest, const void* src, size_t num) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    if (d < s) {
        while (num--) *d++ = *s++;
    } else {
        d += num;
        s += num;
        while (num--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void* ptr1, const void* ptr2, size_t num) {
    const uint8_t* p1 = (const uint8_t*)ptr1;
    const uint8_t* p2 = (const uint8_t*)ptr2;
    
    while (num--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    
    return 0;
}