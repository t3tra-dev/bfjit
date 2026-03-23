#ifndef BFLEXER_H
#define BFLEXER_H

#include <stddef.h>

typedef enum bf_token_kind {
    BF_TOKEN_EOF = 0,
    BF_TOKEN_PTR_INC,
    BF_TOKEN_PTR_DEC,
    BF_TOKEN_DATA_INC,
    BF_TOKEN_DATA_DEC,
    BF_TOKEN_OUTPUT,
    BF_TOKEN_INPUT,
    BF_TOKEN_LOOP_BEGIN,
    BF_TOKEN_LOOP_END
} bf_token_kind;

typedef struct bf_src_loc {
    size_t offset;
    size_t line;
    size_t column;
} bf_src_loc;

typedef struct bf_token {
    bf_token_kind kind;
    bf_src_loc loc;
    char spelling;
} bf_token;

typedef struct bf_lexer {
    const char *src;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
} bf_lexer;

void bf_lexer_init(bf_lexer *lexer, const char *src, size_t length);
bf_token bf_lexer_next(bf_lexer *lexer);
const char *bf_token_kind_name(bf_token_kind kind);

#endif
