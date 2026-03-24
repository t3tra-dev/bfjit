#include "bfparser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bf_parser {
    bf_lexer lexer;
    bf_token current;
    bf_parse_err *err;
} bf_parser;

static void bf_ir_block_reset(bf_ir_block *block) {
    block->nodes = NULL;
    block->count = 0;
    block->capacity = 0;
}

static void bf_parse_err_reset(bf_parse_err *err) {
    if (err == NULL) {
        return;
    }

    err->has_err = false;
    err->loc = (bf_src_loc){0};
    err->msg[0] = '\0';
}

static void bf_set_parse_err(bf_parse_err *err, bf_src_loc loc,
                             const char *msg) {
    if (err == NULL) {
        return;
    }

    err->has_err = true;
    err->loc = loc;
    snprintf(err->msg, sizeof(err->msg), "%s", msg);
}

static void bf_program_reset(bf_program *program) {
    bf_ir_block_reset(&program->root);
}

static void bf_parser_advance(bf_parser *parser) {
    parser->current = bf_lexer_next(&parser->lexer);
}

void bf_ir_block_dispose(bf_ir_block *block) {
    size_t index;

    for (index = 0; index < block->count; ++index) {
        if (block->nodes[index].kind == BF_IR_LOOP) {
            bf_ir_block_dispose(&block->nodes[index].body);
        }
        if (block->nodes[index].kind == BF_IR_MULTIPLY_LOOP ||
            block->nodes[index].kind == BF_IR_MULTI_ZERO) {
            free(block->nodes[index].terms);
        }
    }

    free(block->nodes);
    bf_ir_block_reset(block);
}

void bf_program_dispose(bf_program *program) {
    if (program == NULL) {
        return;
    }

    bf_ir_block_dispose(&program->root);
}

static int bf_ir_block_reserve(bf_ir_block *block, size_t capacity) {
    bf_ir_node *nodes;

    if (block->capacity >= capacity) {
        return 1;
    }

    if (block->capacity == 0) {
        block->capacity = 8;
    }

    while (block->capacity < capacity) {
        block->capacity *= 2;
    }

    nodes = realloc(block->nodes, block->capacity * sizeof(*nodes));
    if (nodes == NULL) {
        return 0;
    }

    block->nodes = nodes;
    return 1;
}

static int bf_ir_block_push(bf_ir_block *block, bf_ir_node node) {
    if (!bf_ir_block_reserve(block, block->count + 1)) {
        return 0;
    }

    block->nodes[block->count] = node;
    block->count += 1;
    return 1;
}

static int bf_ir_block_append_delta(bf_ir_block *block, bf_ir_kind kind,
                                    int delta, bf_src_loc loc) {
    bf_ir_node *last;
    bf_ir_node node;

    if (delta == 0) {
        return 1;
    }

    if (block->count > 0) {
        last = &block->nodes[block->count - 1];
        if (last->kind == kind) {
            last->arg += delta;
            if (last->arg == 0) {
                block->count -= 1;
            }
            return 1;
        }
    }

    node.kind = kind;
    node.loc = loc;
    node.arg = delta;
    bf_ir_block_reset(&node.body);
    node.terms = NULL;
    node.term_count = 0;
    return bf_ir_block_push(block, node);
}

static int bf_parse_block(bf_parser *parser, bf_ir_block *block,
                          int inside_loop, bf_src_loc loop_begin_loc) {
    while (parser->current.kind != BF_TOKEN_EOF &&
           parser->current.kind != BF_TOKEN_LOOP_END) {
        const bf_token token = parser->current;
        bf_ir_node node;

        switch (token.kind) {
        case BF_TOKEN_PTR_INC:
            bf_parser_advance(parser);
            if (!bf_ir_block_append_delta(block, BF_IR_ADD_PTR, 1, token.loc)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending pointer operation");
                return 0;
            }
            break;
        case BF_TOKEN_PTR_DEC:
            bf_parser_advance(parser);
            if (!bf_ir_block_append_delta(block, BF_IR_ADD_PTR, -1,
                                          token.loc)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending pointer operation");
                return 0;
            }
            break;
        case BF_TOKEN_DATA_INC:
            bf_parser_advance(parser);
            if (!bf_ir_block_append_delta(block, BF_IR_ADD_DATA, 1,
                                          token.loc)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending data operation");
                return 0;
            }
            break;
        case BF_TOKEN_DATA_DEC:
            bf_parser_advance(parser);
            if (!bf_ir_block_append_delta(block, BF_IR_ADD_DATA, -1,
                                          token.loc)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending data operation");
                return 0;
            }
            break;
        case BF_TOKEN_OUTPUT:
            node.kind = BF_IR_OUTPUT;
            node.loc = token.loc;
            node.arg = 0;
            bf_ir_block_reset(&node.body);
            node.terms = NULL;
            node.term_count = 0;
            bf_parser_advance(parser);
            if (!bf_ir_block_push(block, node)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending output operation");
                return 0;
            }
            break;
        case BF_TOKEN_INPUT:
            node.kind = BF_IR_INPUT;
            node.loc = token.loc;
            node.arg = 0;
            bf_ir_block_reset(&node.body);
            node.terms = NULL;
            node.term_count = 0;
            bf_parser_advance(parser);
            if (!bf_ir_block_push(block, node)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending input operation");
                return 0;
            }
            break;
        case BF_TOKEN_LOOP_BEGIN:
            node.kind = BF_IR_LOOP;
            node.loc = token.loc;
            node.arg = 0;
            bf_ir_block_reset(&node.body);
            node.terms = NULL;
            node.term_count = 0;
            bf_parser_advance(parser);
            if (!bf_parse_block(parser, &node.body, 1, token.loc)) {
                bf_ir_block_dispose(&node.body);
                return 0;
            }
            if (parser->current.kind != BF_TOKEN_LOOP_END) {
                bf_set_parse_err(parser->err, token.loc, "unterminated loop");
                bf_ir_block_dispose(&node.body);
                return 0;
            }
            bf_parser_advance(parser);
            if (!bf_ir_block_push(block, node)) {
                bf_set_parse_err(
                    parser->err, token.loc,
                    "out of memory while appending loop operation");
                bf_ir_block_dispose(&node.body);
                return 0;
            }
            break;
        default:
            bf_set_parse_err(parser->err, token.loc,
                             "unexpected token while parsing");
            return 0;
        }
    }

    if (parser->current.kind == BF_TOKEN_LOOP_END && !inside_loop) {
        bf_set_parse_err(parser->err, parser->current.loc,
                         "unexpected closing bracket");
        return 0;
    }

    if (parser->current.kind == BF_TOKEN_EOF && inside_loop) {
        bf_set_parse_err(parser->err, loop_begin_loc, "unterminated loop");
        return 0;
    }

    return 1;
}

bool bf_parse_src(const char *src, size_t length, bf_program *program,
                  bf_parse_err *err) {
    bf_parser parser;

    if (program == NULL || src == NULL) {
        if (err != NULL) {
            bf_set_parse_err(err, (bf_src_loc){0},
                             "src and program must be non-null");
        }
        return false;
    }

    bf_program_reset(program);
    bf_parse_err_reset(err);

    bf_lexer_init(&parser.lexer, src, length);
    parser.err = err;
    bf_parser_advance(&parser);

    if (!bf_parse_block(&parser, &program->root, 0, (bf_src_loc){0})) {
        bf_program_dispose(program);
        return false;
    }

    return true;
}

const char *bf_ir_kind_name(bf_ir_kind kind) {
    switch (kind) {
    case BF_IR_ADD_PTR:
        return "add_ptr";
    case BF_IR_ADD_DATA:
        return "add_data";
    case BF_IR_INPUT:
        return "input";
    case BF_IR_OUTPUT:
        return "output";
    case BF_IR_LOOP:
        return "loop";
    case BF_IR_SET_ZERO:
        return "set_zero";
    case BF_IR_SET_CONST:
        return "set_const";
    case BF_IR_MULTI_ZERO:
        return "multi_zero";
    case BF_IR_SCAN:
        return "scan";
    case BF_IR_MULTIPLY_LOOP:
        return "multiply_loop";
    default:
        return "unknown";
    }
}
