#include "bflexer.h"

static int bf_is_inst(char ch) {
    switch (ch) {
    case '>':
    case '<':
    case '+':
    case '-':
    case '.':
    case ',':
    case '[':
    case ']':
        return 1;
    default:
        return 0;
    }
}

static void bf_lexer_adv(bf_lexer *lexer, char ch) {
    lexer->offset += 1;

    if (ch == '\n') {
        lexer->line += 1;
        lexer->column = 1;
        return;
    }

    lexer->column += 1;
}

void bf_lexer_init(bf_lexer *lexer, const char *src, size_t length) {
    lexer->src = src;
    lexer->length = length;
    lexer->offset = 0;
    lexer->line = 1;
    lexer->column = 1;
}

bf_token bf_lexer_next(bf_lexer *lexer) {
    while (lexer->offset < lexer->length) {
        const char ch = lexer->src[lexer->offset];
        const bf_src_loc loc = {
            .offset = lexer->offset,
            .line = lexer->line,
            .column = lexer->column,
        };

        bf_lexer_adv(lexer, ch);

        if (!bf_is_inst(ch)) {
            continue;
        }

        switch (ch) {
        case '>':
            return (bf_token){
                .kind = BF_TOKEN_PTR_INC, .loc = loc, .spelling = ch};
        case '<':
            return (bf_token){
                .kind = BF_TOKEN_PTR_DEC, .loc = loc, .spelling = ch};
        case '+':
            return (bf_token){
                .kind = BF_TOKEN_DATA_INC, .loc = loc, .spelling = ch};
        case '-':
            return (bf_token){
                .kind = BF_TOKEN_DATA_DEC, .loc = loc, .spelling = ch};
        case '.':
            return (bf_token){
                .kind = BF_TOKEN_OUTPUT, .loc = loc, .spelling = ch};
        case ',':
            return (bf_token){
                .kind = BF_TOKEN_INPUT, .loc = loc, .spelling = ch};
        case '[':
            return (bf_token){
                .kind = BF_TOKEN_LOOP_BEGIN, .loc = loc, .spelling = ch};
        case ']':
            return (bf_token){
                .kind = BF_TOKEN_LOOP_END, .loc = loc, .spelling = ch};
        default:
            break;
        }
    }

    return (bf_token){
        .kind = BF_TOKEN_EOF,
        .loc =
            {
                .offset = lexer->offset,
                .line = lexer->line,
                .column = lexer->column,
            },
        .spelling = '\0',
    };
}

const char *bf_token_kind_name(bf_token_kind kind) {
    switch (kind) {
    case BF_TOKEN_EOF:
        return "eof";
    case BF_TOKEN_PTR_INC:
        return "ptr_inc";
    case BF_TOKEN_PTR_DEC:
        return "ptr_dec";
    case BF_TOKEN_DATA_INC:
        return "data_inc";
    case BF_TOKEN_DATA_DEC:
        return "data_dec";
    case BF_TOKEN_OUTPUT:
        return "output";
    case BF_TOKEN_INPUT:
        return "input";
    case BF_TOKEN_LOOP_BEGIN:
        return "loop_begin";
    case BF_TOKEN_LOOP_END:
        return "loop_end";
    default:
        return "unknown";
    }
}
