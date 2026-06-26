#ifndef STRING_H
#define STRING_H

#include "lib/types.h"

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int strcmp(const char* str1, const char* str2);
char* strchr(const char* str, int c);
char* strtok(char* str, const char* delim);
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t num);
int strncmp(const char* str1, const char* str2, size_t num);
char* strncpy(char* dest, const char* src, size_t num);
char* strrchr(const char* str, int c);
void* memmove(void* dest, const void* src, size_t num);
int memcmp(const void* s1, const void* s2, size_t n);

#endif