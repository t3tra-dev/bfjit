#include "bfopt.h"

#include <stdlib.h>
#include <string.h>

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

static void bf_opt_fold_set_const(bf_ir_block *block) {
    size_t index;

    for (index = 0; index + 1 < block->count; ++index) {
        bf_ir_node *node;
        bf_ir_node *next;

        node = &block->nodes[index];
        next = &block->nodes[index + 1];

        if (node->kind == BF_IR_SET_ZERO && next->kind == BF_IR_ADD_DATA) {
            node->kind = BF_IR_SET_CONST;
            node->arg = next->arg;
            memmove(&block->nodes[index + 1], &block->nodes[index + 2],
                    (block->count - index - 2) * sizeof(*block->nodes));
            block->count -= 1;
            index -= (index > 0);
        } else if (node->kind == BF_IR_SET_CONST &&
                   next->kind == BF_IR_ADD_DATA) {
            node->arg += next->arg;
            if (node->arg == 0) {
                node->kind = BF_IR_SET_ZERO;
            }
            memmove(&block->nodes[index + 1], &block->nodes[index + 2],
                    (block->count - index - 2) * sizeof(*block->nodes));
            block->count -= 1;
            index -= (index > 0);
        }
    }
}

typedef struct bf_store_record {
    int offset;
    size_t node_index;
} bf_store_record;

static size_t bf_find_store_record(const bf_store_record *records,
                                   size_t record_count, int offset) {
    size_t index;

    for (index = 0; index < record_count; ++index) {
        if (records[index].offset == offset) {
            return index;
        }
    }

    return record_count;
}

static void bf_drop_store_record(bf_store_record *records, size_t *record_count,
                                 int offset) {
    size_t record_index;

    record_index = bf_find_store_record(records, *record_count, offset);
    if (record_index == *record_count) {
        return;
    }

    memmove(&records[record_index], &records[record_index + 1],
            (*record_count - record_index - 1) * sizeof(*records));
    *record_count -= 1;
}

static void bf_kill_prior_store(bf_store_record *records, size_t *record_count,
                                int offset, unsigned char *remove_mask) {
    size_t record_index;

    record_index = bf_find_store_record(records, *record_count, offset);
    if (record_index == *record_count) {
        return;
    }

    remove_mask[records[record_index].node_index] = 1;
    memmove(&records[record_index], &records[record_index + 1],
            (*record_count - record_index - 1) * sizeof(*records));
    *record_count -= 1;
}

static void bf_record_store(bf_store_record *records, size_t *record_count,
                            int offset, size_t node_index,
                            unsigned char *remove_mask) {
    size_t record_index;

    record_index = bf_find_store_record(records, *record_count, offset);
    if (record_index < *record_count) {
        remove_mask[records[record_index].node_index] = 1;
        records[record_index].node_index = node_index;
        return;
    }

    records[*record_count].offset = offset;
    records[*record_count].node_index = node_index;
    *record_count += 1;
}

static void bf_clear_store_records(size_t *record_count) { *record_count = 0; }

static void bf_opt_eliminate_dead_stores(bf_ir_block *block) {
    bf_store_record *records;
    unsigned char *remove_mask;
    size_t record_count;
    size_t index;
    size_t write_index;
    int current_offset;
    int changed;

    if (block->count < 2) {
        return;
    }

    records = malloc(block->count * sizeof(*records));
    remove_mask = calloc(block->count, sizeof(*remove_mask));
    if (records == NULL || remove_mask == NULL) {
        free(records);
        free(remove_mask);
        return;
    }

    record_count = 0;
    current_offset = 0;
    changed = 0;

    for (index = 0; index < block->count; ++index) {
        bf_ir_node *node;

        node = &block->nodes[index];
        switch (node->kind) {
        case BF_IR_ADD_PTR:
            current_offset += node->arg;
            break;
        case BF_IR_SET_ZERO:
        case BF_IR_SET_CONST:
            bf_record_store(records, &record_count, current_offset, index,
                            remove_mask);
            break;
        case BF_IR_INPUT:
            bf_kill_prior_store(records, &record_count, current_offset,
                                remove_mask);
            break;
        case BF_IR_ADD_DATA:
        case BF_IR_OUTPUT:
            bf_drop_store_record(records, &record_count, current_offset);
            break;
        case BF_IR_MULTI_ZERO: {
            size_t term_index;

            for (term_index = 0; term_index < node->term_count; ++term_index) {
                bf_kill_prior_store(records, &record_count,
                                    current_offset +
                                        node->terms[term_index].offset,
                                    remove_mask);
            }
            current_offset += node->arg;
            break;
        }
        case BF_IR_LOOP:
        case BF_IR_SCAN:
        case BF_IR_MULTIPLY_LOOP:
            bf_clear_store_records(&record_count);
            break;
        default:
            bf_clear_store_records(&record_count);
            break;
        }
    }

    for (index = 0; index < block->count; ++index) {
        if (remove_mask[index] != 0) {
            changed = 1;
            break;
        }
    }

    if (changed) {
        write_index = 0;
        for (index = 0; index < block->count; ++index) {
            if (remove_mask[index] == 0) {
                if (write_index != index) {
                    block->nodes[write_index] = block->nodes[index];
                }
                write_index += 1;
            }
        }
        block->count = write_index;
    }

    free(records);
    free(remove_mask);
}

static void bf_try_fold_multi_zero(bf_ir_block *block) {
    size_t index;

    for (index = 0; index < block->count; ++index) {
        size_t scan_index;
        size_t zero_count;
        int current_offset;
        bf_multiply_term *offsets;

        if (block->nodes[index].kind != BF_IR_SET_ZERO) {
            continue;
        }

        zero_count = 1;
        current_offset = 0;
        scan_index = index + 1;
        while (scan_index + 1 < block->count &&
               block->nodes[scan_index].kind == BF_IR_ADD_PTR &&
               block->nodes[scan_index + 1].kind == BF_IR_SET_ZERO) {
            current_offset += block->nodes[scan_index].arg;
            zero_count += 1;
            scan_index += 2;
        }

        if (zero_count < 2) {
            continue;
        }

        offsets = malloc(zero_count * sizeof(*offsets));
        if (offsets == NULL) {
            continue;
        }

        offsets[0].offset = 0;
        offsets[0].factor = 0;
        current_offset = 0;
        zero_count = 1;
        scan_index = index + 1;
        while (scan_index + 1 < block->count &&
               block->nodes[scan_index].kind == BF_IR_ADD_PTR &&
               block->nodes[scan_index + 1].kind == BF_IR_SET_ZERO) {
            current_offset += block->nodes[scan_index].arg;
            offsets[zero_count].offset = current_offset;
            offsets[zero_count].factor = 0;
            zero_count += 1;
            scan_index += 2;
        }

        if (scan_index < block->count &&
            block->nodes[scan_index].kind == BF_IR_ADD_PTR) {
            current_offset += block->nodes[scan_index].arg;
            scan_index += 1;
        }

        block->nodes[index].kind = BF_IR_MULTI_ZERO;
        block->nodes[index].arg = current_offset;
        block->nodes[index].terms = offsets;
        block->nodes[index].term_count = zero_count;

        memmove(&block->nodes[index + 1], &block->nodes[scan_index],
                (block->count - scan_index) * sizeof(*block->nodes));
        block->count -= (scan_index - index - 1);
    }
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

    bf_opt_fold_set_const(block);
    bf_opt_eliminate_dead_stores(block);
    bf_try_fold_multi_zero(block);
    bf_opt_eliminate_dead_stores(block);
}

void bf_opt_program(bf_program *program) {
    if (program == NULL) {
        return;
    }

    bf_opt_block(&program->root);
}
