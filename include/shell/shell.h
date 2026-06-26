#ifndef SHELL_H
#define SHELL_H

#include "lib/types.h"

#define MAX_COMMAND_LENGTH 256
#define MAX_ARGUMENTS      16

typedef struct shell_command {
    const char* name;
    const char* help;
    int (*func)(int argc, char** argv);
} shell_command_t;

void shell_init();
void shell_run();

#endif