#ifndef STRING_H
#define STRING_H

#include "types.h"

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int strcmp(const char* str1, const char* str2);
char* strchr(const char* str, int c);
char* strtok(char* str, const char* delim);
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t num);

#endif