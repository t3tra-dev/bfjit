#include "bfopt.h"

#include <stdlib.h>

static void bf_opt_set_zero(bf_ir_node *node) {
    const bf_ir_block *body;

    if (node->kind != BF_IR_LOOP || node->body.count != 1) {
        return;
    }

    body = &node->body;

    /* [-] or [+]: ループ消去 */
    if (body->nodes[0].kind == BF_IR_ADD_DATA &&
        (body->nodes[0].arg == -1 || body->nodes[0].arg == 1)) {
        bf_ir_block_dispose(&node->body);
        node->kind = BF_IR_SET_ZERO;
        node->arg = 0;
        return;
    }

    /* [>] or [<] or [>>>] etc: ループ解析 */
    if (body->nodes[0].kind == BF_IR_ADD_PTR) {
        int step = body->nodes[0].arg;
        bf_ir_block_dispose(&node->body);
        node->kind = BF_IR_SCAN;
        node->arg = step;
        return;
    }
}

/*
 * 乗算/コピー ループパターンの検出を試行
 *
 * 乗算ループは [->+++>++<<] のような形をしており, ループ本体は ADD_PTR および
 * ADD_DATA のみで構成され, ポインタはオフセット 0 に戻りオフセット 0 はちょうど
 * 1 だけデクリメント (または 1 だけインクリメント,
 * これはラップアラウンドで動作) される
 *
 * 成功すると LOOP は MULTIPLY_LOOP に置き換えられ、terms 配列に各影響セルの
 * {offset, factor} ペアが格納される
 */
#define BF_MULTIPLY_MAX_OFFSETS 256

static void bf_try_recognize_multiply_loop(bf_ir_node *node) {
    const bf_ir_block *body;
    size_t i;
    int current_offset;
    int ptr_sum;
    int origin_delta;
    int sign;

    /* オフセット -> 累積デルタ */
    int offsets[BF_MULTIPLY_MAX_OFFSETS];
    int offset_keys[BF_MULTIPLY_MAX_OFFSETS];
    size_t offset_count;

    bf_multiply_term *terms;
    size_t term_count;

    if (node->kind != BF_IR_LOOP) {
        return;
    }

    body = &node->body;

    /* 1. body には ADD_PTR と ADD_DATA のみを含める */
    for (i = 0; i < body->count; ++i) {
        if (body->nodes[i].kind != BF_IR_ADD_PTR &&
            body->nodes[i].kind != BF_IR_ADD_DATA) {
            return;
        }
    }

    /* 2. body を走査しポインタのオフセットを追跡, delta を累積 */
    current_offset = 0;
    ptr_sum = 0;
    offset_count = 0;

    for (i = 0; i < body->count; ++i) {
        if (body->nodes[i].kind == BF_IR_ADD_PTR) {
            current_offset += body->nodes[i].arg;
            ptr_sum += body->nodes[i].arg;
        } else {
            /* current_offset で ADD_DATA */
            size_t j;
            int found = 0;

            for (j = 0; j < offset_count; ++j) {
                if (offset_keys[j] == current_offset) {
                    offsets[j] += body->nodes[i].arg;
                    found = 1;
                    break;
                }
            }

            if (!found) {
                if (offset_count >= BF_MULTIPLY_MAX_OFFSETS) {
                    return;
                }
                offset_keys[offset_count] = current_offset;
                offsets[offset_count] = body->nodes[i].arg;
                offset_count += 1;
            }
        }
    }

    /* 3. ポインタはオフセット 0 に戻る */
    if (ptr_sum != 0) {
        return;
    }

    /* 4. オフセット 0 は -1 または +1 の delta を持つ */
    origin_delta = 0;
    for (i = 0; i < offset_count; ++i) {
        if (offset_keys[i] == 0) {
            origin_delta = offsets[i];
            break;
        }
    }

    if (origin_delta != -1 && origin_delta != 1) {
        return;
    }

    /*
     * 5. terms 配列を構築
     * origin_delta が +1 の場合, すべての係数を反転させてセマンティクスを
     * cell[off] += cell[0] * factor (cell[0] は 0 にデクリメントされる) にする
     */
    sign = (origin_delta == -1) ? 1 : -1;

    term_count = 0;
    for (i = 0; i < offset_count; ++i) {
        if (offset_keys[i] != 0) {
            term_count += 1;
        }
    }

    terms = NULL;
    if (term_count > 0) {
        terms = malloc(term_count * sizeof(*terms));
        if (terms == NULL) {
            return;
        }

        term_count = 0;
        for (i = 0; i < offset_count; ++i) {
            if (offset_keys[i] != 0) {
                terms[term_count].offset = offset_keys[i];
                terms[term_count].factor = offsets[i] * sign;
                term_count += 1;
            }
        }
    }

    /* 6. ノードを変換 */
    bf_ir_block_dispose(&node->body);
    node->kind = BF_IR_MULTIPLY_LOOP;
    node->arg = 0;
    node->terms = terms;
    node->term_count = term_count;
}

static void bf_opt_block(bf_ir_block *block) {
    size_t i;

    for (i = 0; i < block->count; ++i) {
        if (block->nodes[i].kind == BF_IR_LOOP) {
            bf_opt_block(&block->nodes[i].body);
            bf_opt_set_zero(&block->nodes[i]);
            bf_try_recognize_multiply_loop(&block->nodes[i]);
        }
    }
}

void bf_opt_program(bf_program *program) {
    if (program == NULL) {
        return;
    }

    bf_opt_block(&program->root);
}
