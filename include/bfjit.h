#ifndef BFJIT_H
#define BFJIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bfparser.h"

typedef struct bf_jit_err {
    bool has_err;
    char msg[512];
} bf_jit_err;

bool bf_jit_execute_program(const bf_program *program, uint8_t *tape,
                            size_t tape_size, bf_jit_err *err);

#endif // BFJIT_H
