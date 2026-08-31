#ifndef STDLIB_H
#define STDLIB_H

#include "lib/types.h"

#include <stdarg.h>

int atoi(const char* str);
uint32_t strtoul(const char* str, char** endptr, int base);

void itoa(int num, char* str, int base, int max_size);
void utoa(uint32_t num, char* str, int base, int max_size);

/* bounded snprintf subset: %s %d %u %x %c %%; NUL-terminates */
void ksprintf(char* buf, size_t size, const char* fmt, ...);

void srand(uint32_t seed);
uint32_t rand();

#endif