#ifndef STDLIB_H
#define STDLIB_H

#include "types.h"

int atoi(const char* str);
uint32_t strtoul(const char* str, char** endptr, int base);

void itoa(int num, char* str, int base);
void utoa(uint32_t num, char* str, int base);

#endif