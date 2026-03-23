#ifndef BFPARSER_H
#define BFPARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "bflexer.h"

typedef enum bf_ir_kind {
    BF_IR_ADD_PTR = 0,
    BF_IR_ADD_DATA,
    BF_IR_INPUT,
    BF_IR_OUTPUT,
    BF_IR_LOOP,
    BF_IR_SET_ZERO,
    BF_IR_SCAN,
    BF_IR_MULTIPLY_LOOP
} bf_ir_kind;

typedef struct bf_multiply_term {
    int offset;
    int factor;
} bf_multiply_term;

typedef struct bf_ir_node bf_ir_node;

typedef struct bf_ir_block {
    bf_ir_node *nodes;
    size_t count;
    size_t capacity;
} bf_ir_block;

struct bf_ir_node {
    bf_ir_kind kind;
    bf_src_loc loc;
    int arg;
    bf_ir_block body;
    bf_multiply_term *terms;
    size_t term_count;
};

typedef struct bf_program {
    bf_ir_block root;
} bf_program;

typedef struct bf_parse_err {
    bool has_err;
    bf_src_loc loc;
    char msg[256];
} bf_parse_err;

bool bf_parse_src(const char *src, size_t length, bf_program *program,
                  bf_parse_err *err);
void bf_program_dispose(bf_program *program);
void bf_ir_block_dispose(bf_ir_block *block);
const char *bf_ir_kind_name(bf_ir_kind kind);

#endif
