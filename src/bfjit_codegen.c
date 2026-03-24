#define _GNU_SOURCE

#include "bfjit_internal.h"

#include <stdint.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

typedef struct bf_codegen {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMValueRef func;
    LLVMBasicBlockRef oob_block;
    LLVMValueRef oob_reason_ptr;
    LLVMValueRef tape_arg;
    LLVMValueRef tape_size_arg;
    LLVMValueRef pointer_index;
    LLVMValueRef memchr_func;
    LLVMValueRef scan_index_func;
    LLVMValueRef scan_index_step4_func;
    LLVMTypeRef i1_type;
    LLVMTypeRef i8_type;
    LLVMTypeRef i32_type;
    LLVMTypeRef i64_type;
    LLVMTypeRef tape_pointer_type;
    LLVMValueRef putchar_func;
    LLVMValueRef getchar_func;
    bf_jit_err *err;
    int suppress_ptr_guards;
} bf_codegen;

typedef struct bf_block_bounds {
    int min_offset;
    int max_offset;
    int final_offset;
    int has_motion;
} bf_block_bounds;

static LLVMValueRef bf_const_i8(bf_codegen *codegen, int value) {
    return LLVMConstInt(codegen->i8_type, (unsigned long long)(uint8_t)value,
                        0);
}

static LLVMValueRef bf_const_i64(bf_codegen *codegen, uint64_t value) {
    return LLVMConstInt(codegen->i64_type, value, 0);
}

static LLVMValueRef bf_const_i32(bf_codegen *codegen, uint32_t value) {
    return LLVMConstInt(codegen->i32_type, (unsigned long long)value, 0);
}

static void bf_add_enum_attr_if_known(LLVMContextRef ctx, LLVMValueRef value,
                                      LLVMAttributeIndex index,
                                      const char *name, size_t name_length) {
    unsigned kind;

    kind = LLVMGetEnumAttributeKindForName(name, name_length);
    if (kind == 0) {
        return;
    }

    LLVMAddAttributeAtIndex(value, index,
                            LLVMCreateEnumAttribute(ctx, kind, 0));
}

static LLVMBasicBlockRef bf_get_oob_block(bf_codegen *codegen) {
    if (codegen->oob_block == NULL) {
        codegen->oob_block =
            LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, "oob");
    }

    return codegen->oob_block;
}

static void bf_build_bounds_guard(bf_codegen *codegen, LLVMValueRef index,
                                  const char *block_name, uint32_t reason) {
    LLVMValueRef in_bounds;
    LLVMBasicBlockRef continue_block;
    LLVMBasicBlockRef fail_block;

    in_bounds = LLVMBuildICmp(codegen->builder, LLVMIntULT, index,
                              codegen->tape_size_arg, "in_bounds");
    continue_block =
        LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, block_name);
    fail_block =
        LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, "oob.set");
    LLVMBuildCondBr(codegen->builder, in_bounds, continue_block, fail_block);

    LLVMPositionBuilderAtEnd(codegen->builder, fail_block);
    LLVMBuildStore(codegen->builder, bf_const_i32(codegen, reason),
                   codegen->oob_reason_ptr);
    LLVMBuildBr(codegen->builder, bf_get_oob_block(codegen));

    LLVMPositionBuilderAtEnd(codegen->builder, continue_block);
}

static LLVMValueRef bf_build_current_cell_ptr(bf_codegen *codegen) {
    LLVMValueRef indices[1];

    indices[0] = codegen->pointer_index;

    return LLVMBuildGEP2(codegen->builder, codegen->i8_type, codegen->tape_arg,
                         indices, 1, "cell_ptr");
}

static void bf_bounds_include(int value, int *min_value, int *max_value) {
    if (value < *min_value) {
        *min_value = value;
    }
    if (value > *max_value) {
        *max_value = value;
    }
}

static int bf_analyze_block_bounds_range(const bf_ir_block *block,
                                         size_t start_index, size_t end_index,
                                         bf_block_bounds *bounds) {
    size_t index;
    int current_offset;
    int min_offset;
    int max_offset;
    int has_motion;

    current_offset = 0;
    min_offset = 0;
    max_offset = 0;
    has_motion = 0;

    for (index = start_index; index < end_index; ++index) {
        const bf_ir_node *node;

        node = &block->nodes[index];
        switch (node->kind) {
        case BF_IR_ADD_PTR:
            current_offset += node->arg;
            has_motion = 1;
            bf_bounds_include(current_offset, &min_offset, &max_offset);
            break;
        case BF_IR_ADD_DATA:
        case BF_IR_INPUT:
        case BF_IR_OUTPUT:
        case BF_IR_SET_ZERO:
        case BF_IR_SET_CONST:
            bf_bounds_include(current_offset, &min_offset, &max_offset);
            break;
        case BF_IR_MULTI_ZERO: {
            size_t term_index;

            for (term_index = 0; term_index < node->term_count; ++term_index) {
                bf_bounds_include(current_offset +
                                      node->terms[term_index].offset,
                                  &min_offset, &max_offset);
            }
            current_offset += node->arg;
            if (node->arg != 0) {
                has_motion = 1;
                bf_bounds_include(current_offset, &min_offset, &max_offset);
            }
            break;
        }
        case BF_IR_MULTIPLY_LOOP: {
            size_t term_index;

            bf_bounds_include(current_offset, &min_offset, &max_offset);
            for (term_index = 0; term_index < node->term_count; ++term_index) {
                bf_bounds_include(current_offset +
                                      node->terms[term_index].offset,
                                  &min_offset, &max_offset);
            }
            break;
        }
        case BF_IR_LOOP: {
            bf_block_bounds nested_bounds;

            if (!bf_analyze_block_bounds_range(&node->body, 0, node->body.count,
                                               &nested_bounds) ||
                nested_bounds.final_offset != 0) {
                return 0;
            }

            bf_bounds_include(current_offset, &min_offset, &max_offset);
            bf_bounds_include(current_offset + nested_bounds.min_offset,
                              &min_offset, &max_offset);
            bf_bounds_include(current_offset + nested_bounds.max_offset,
                              &min_offset, &max_offset);
            if (nested_bounds.has_motion) {
                has_motion = 1;
            }
            break;
        }
        case BF_IR_SCAN:
            return 0;
        default:
            return 0;
        }
    }

    bounds->min_offset = min_offset;
    bounds->max_offset = max_offset;
    bounds->final_offset = current_offset;
    bounds->has_motion = has_motion;
    return 1;
}

static int bf_analyze_block_bounds(const bf_ir_block *block,
                                   bf_block_bounds *bounds) {
    return bf_analyze_block_bounds_range(block, 0, block->count, bounds);
}

static int bf_find_guarded_range(const bf_ir_block *block, size_t start_index,
                                 size_t *end_index, bf_block_bounds *bounds) {
    size_t candidate_end;
    int found;

    found = 0;
    for (candidate_end = start_index + 1; candidate_end <= block->count;
         ++candidate_end) {
        bf_block_bounds candidate_bounds;

        if (!bf_analyze_block_bounds_range(block, start_index, candidate_end,
                                           &candidate_bounds)) {
            break;
        }

        if (candidate_bounds.final_offset == 0 && candidate_bounds.has_motion &&
            (candidate_bounds.min_offset != 0 ||
             candidate_bounds.max_offset != 0) &&
            candidate_end > start_index + 1) {
            *end_index = candidate_end;
            *bounds = candidate_bounds;
            found = 1;
        }
    }

    return found;
}

static void bf_build_relative_bounds_guard(bf_codegen *codegen,
                                           LLVMValueRef base_index,
                                           int min_offset, int max_offset,
                                           uint32_t min_reason,
                                           uint32_t max_reason) {
    if (min_offset < 0) {
        LLVMValueRef min_index;

        min_index = LLVMBuildAdd(
            codegen->builder, base_index,
            LLVMConstInt(codegen->i64_type,
                         (unsigned long long)(int64_t)min_offset, 1),
            "loop_guard_min_idx");
        bf_build_bounds_guard(codegen, min_index, "loop.min.in_bounds",
                              min_reason);
    }

    if (max_offset > 0) {
        LLVMValueRef max_index;

        max_index = LLVMBuildAdd(
            codegen->builder, base_index,
            LLVMConstInt(codegen->i64_type,
                         (unsigned long long)(int64_t)max_offset, 1),
            "loop_guard_max_idx");
        bf_build_bounds_guard(codegen, max_index, "loop.max.in_bounds",
                              max_reason);
    }
}

static int bf_codegen_block(bf_codegen *codegen, const bf_ir_block *block);

static int bf_codegen_loop(bf_codegen *codegen, const bf_ir_node *node) {
    LLVMBasicBlockRef pre_loop_block;
    LLVMBasicBlockRef initial_block;
    LLVMValueRef pre_loop_ptr;
    LLVMBasicBlockRef condition_block;
    LLVMBasicBlockRef body_block;
    LLVMBasicBlockRef exit_block;
    LLVMValueRef ptr_phi;
    LLVMValueRef cell_ptr;
    LLVMValueRef cell_value;
    LLVMValueRef loop_condition;
    bf_block_bounds loop_bounds;
    int use_loop_guard;
    LLVMBasicBlockRef body_end_block;
    LLVMValueRef body_end_ptr;

    pre_loop_block = LLVMGetInsertBlock(codegen->builder);
    pre_loop_ptr = codegen->pointer_index;

    condition_block =
        LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, "loop.cond");
    body_block =
        LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, "loop.body");
    exit_block =
        LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func, "loop.exit");

    use_loop_guard = bf_analyze_block_bounds(&node->body, &loop_bounds) &&
                     loop_bounds.final_offset == 0 && loop_bounds.has_motion;
    if (use_loop_guard) {
        LLVMBasicBlockRef guard_block;

        guard_block = LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func,
                                                    "loop.guard");
        LLVMBuildBr(codegen->builder, guard_block);
        LLVMPositionBuilderAtEnd(codegen->builder, guard_block);
        bf_build_relative_bounds_guard(codegen, pre_loop_ptr,
                                       loop_bounds.min_offset,
                                       loop_bounds.max_offset, 6, 7);
        initial_block = LLVMGetInsertBlock(codegen->builder);
        LLVMBuildBr(codegen->builder, condition_block);
    } else {
        LLVMBuildBr(codegen->builder, condition_block);
        initial_block = pre_loop_block;
    }

    LLVMPositionBuilderAtEnd(codegen->builder, condition_block);
    ptr_phi = LLVMBuildPhi(codegen->builder, codegen->i64_type, "ptr.phi");
    LLVMAddIncoming(ptr_phi, &pre_loop_ptr, &initial_block, 1);
    codegen->pointer_index = ptr_phi;

    cell_ptr = bf_build_current_cell_ptr(codegen);
    cell_value = LLVMBuildLoad2(codegen->builder, codegen->i8_type, cell_ptr,
                                "loop_value");
    loop_condition = LLVMBuildICmp(codegen->builder, LLVMIntNE, cell_value,
                                   bf_const_i8(codegen, 0), "loop_condition");
    LLVMBuildCondBr(codegen->builder, loop_condition, body_block, exit_block);

    LLVMPositionBuilderAtEnd(codegen->builder, body_block);
    if (use_loop_guard) {
        codegen->suppress_ptr_guards += 1;
    }
    if (!bf_codegen_block(codegen, &node->body)) {
        return 0;
    }
    if (use_loop_guard) {
        codegen->suppress_ptr_guards -= 1;
    }

    body_end_block = LLVMGetInsertBlock(codegen->builder);
    body_end_ptr = codegen->pointer_index;

    if (LLVMGetBasicBlockTerminator(body_end_block) == NULL) {
        LLVMBuildBr(codegen->builder, condition_block);
    }

    LLVMAddIncoming(ptr_phi, &body_end_ptr, &body_end_block, 1);

    LLVMPositionBuilderAtEnd(codegen->builder, exit_block);
    codegen->pointer_index = ptr_phi;
    return 1;
}

static int bf_codegen_node(bf_codegen *codegen, const bf_ir_node *node) {
    LLVMValueRef cell_ptr;
    LLVMValueRef current_value;
    LLVMValueRef updated_value;
    LLVMValueRef call_args[1];

    switch (node->kind) {
    case BF_IR_ADD_PTR:
        updated_value = LLVMBuildAdd(
            codegen->builder, codegen->pointer_index,
            LLVMConstInt(codegen->i64_type,
                         (unsigned long long)(int64_t)node->arg, 1),
            "ptr_next");
        if (codegen->suppress_ptr_guards == 0) {
            bf_build_bounds_guard(codegen, updated_value, "ptr.in_bounds", 1);
        }
        codegen->pointer_index = updated_value;
        return 1;
    case BF_IR_ADD_DATA:
        cell_ptr = bf_build_current_cell_ptr(codegen);
        current_value = LLVMBuildLoad2(codegen->builder, codegen->i8_type,
                                       cell_ptr, "cell_value");
        updated_value =
            LLVMBuildAdd(codegen->builder, current_value,
                         bf_const_i8(codegen, node->arg), "cell_next");
        LLVMBuildStore(codegen->builder, updated_value, cell_ptr);
        return 1;
    case BF_IR_OUTPUT:
        cell_ptr = bf_build_current_cell_ptr(codegen);
        current_value = LLVMBuildLoad2(codegen->builder, codegen->i8_type,
                                       cell_ptr, "output_value");
        call_args[0] = LLVMBuildZExt(codegen->builder, current_value,
                                     codegen->i32_type, "output_arg");
        LLVMBuildCall2(codegen->builder,
                       LLVMGlobalGetValueType(codegen->putchar_func),
                       codegen->putchar_func, call_args, 1, "");
        return 1;
    case BF_IR_INPUT:
        cell_ptr = bf_build_current_cell_ptr(codegen);
        current_value = LLVMBuildCall2(
            codegen->builder, LLVMGlobalGetValueType(codegen->getchar_func),
            codegen->getchar_func, NULL, 0, "input_value");
        updated_value = LLVMBuildTrunc(codegen->builder, current_value,
                                       codegen->i8_type, "input_trunc");
        LLVMBuildStore(codegen->builder, updated_value, cell_ptr);
        return 1;
    case BF_IR_LOOP:
        return bf_codegen_loop(codegen, node);
    case BF_IR_SET_ZERO:
        cell_ptr = bf_build_current_cell_ptr(codegen);
        LLVMBuildStore(codegen->builder, bf_const_i8(codegen, 0), cell_ptr);
        return 1;
    case BF_IR_SET_CONST:
        cell_ptr = bf_build_current_cell_ptr(codegen);
        LLVMBuildStore(codegen->builder, bf_const_i8(codegen, node->arg),
                       cell_ptr);
        return 1;
    case BF_IR_MULTI_ZERO: {
        size_t term_index;

        if (codegen->suppress_ptr_guards == 0 && node->term_count > 0) {
            int min_offset;
            int max_offset;

            min_offset = node->terms[0].offset;
            max_offset = node->terms[0].offset;
            for (term_index = 1; term_index < node->term_count; ++term_index) {
                if (node->terms[term_index].offset < min_offset) {
                    min_offset = node->terms[term_index].offset;
                }
                if (node->terms[term_index].offset > max_offset) {
                    max_offset = node->terms[term_index].offset;
                }
            }
            if (node->arg < min_offset) {
                min_offset = node->arg;
            }
            if (node->arg > max_offset) {
                max_offset = node->arg;
            }

            bf_build_relative_bounds_guard(codegen, codegen->pointer_index,
                                           min_offset, max_offset, 8, 9);
        }

        for (term_index = 0; term_index < node->term_count; ++term_index) {
            LLVMValueRef target_idx;
            LLVMValueRef target_indices[1];
            LLVMValueRef target_ptr;

            target_idx = LLVMBuildAdd(
                codegen->builder, codegen->pointer_index,
                LLVMConstInt(
                    codegen->i64_type,
                    (unsigned long long)(int64_t)node->terms[term_index].offset,
                    1),
                "multi_zero_idx");
            target_indices[0] = target_idx;
            target_ptr = LLVMBuildGEP2(codegen->builder, codegen->i8_type,
                                       codegen->tape_arg, target_indices, 1,
                                       "multi_zero_ptr");
            LLVMBuildStore(codegen->builder, bf_const_i8(codegen, 0),
                           target_ptr);
        }

        if (node->arg != 0) {
            codegen->pointer_index = LLVMBuildAdd(
                codegen->builder, codegen->pointer_index,
                LLVMConstInt(codegen->i64_type,
                             (unsigned long long)(int64_t)node->arg, 1),
                "multi_zero_ptr_next");
        }
        return 1;
    }
    case BF_IR_SCAN: {
        if (node->arg == 1) {
            LLVMValueRef search_ptr;
            LLVMValueRef start_value;
            LLVMValueRef start_is_zero;
            LLVMValueRef remaining;
            LLVMValueRef scan_args[3];
            LLVMValueRef result;
            LLVMValueRef found;
            LLVMValueRef result_int;
            LLVMValueRef tape_int;
            LLVMValueRef new_index;
            LLVMValueRef phi;
            LLVMBasicBlockRef pre_block;
            LLVMBasicBlockRef search_block;
            LLVMBasicBlockRef found_block;
            LLVMBasicBlockRef fail_block;
            LLVMBasicBlockRef done_block;
            LLVMValueRef pre_ptr;

            pre_block = LLVMGetInsertBlock(codegen->builder);
            pre_ptr = codegen->pointer_index;

            search_ptr = bf_build_current_cell_ptr(codegen);
            start_value = LLVMBuildLoad2(codegen->builder, codegen->i8_type,
                                         search_ptr, "scan_start_val");
            start_is_zero =
                LLVMBuildICmp(codegen->builder, LLVMIntEQ, start_value,
                              bf_const_i8(codegen, 0), "scan_start_is_zero");
            search_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.search");
            found_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.found");
            fail_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.not_found");
            done_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.done");

            LLVMBuildCondBr(codegen->builder, start_is_zero, done_block,
                            search_block);

            LLVMPositionBuilderAtEnd(codegen->builder, search_block);
            remaining = LLVMBuildSub(codegen->builder, codegen->tape_size_arg,
                                     codegen->pointer_index, "scan_remaining");

            scan_args[0] = search_ptr;
            scan_args[1] = LLVMConstInt(codegen->i32_type, 0, 0);
            scan_args[2] = remaining;
            result = LLVMBuildCall2(
                codegen->builder, LLVMGlobalGetValueType(codegen->memchr_func),
                codegen->memchr_func, scan_args, 3, "scan_memchr");

            found = LLVMBuildICmp(codegen->builder, LLVMIntNE, result,
                                  LLVMConstNull(codegen->tape_pointer_type),
                                  "scan_found");

            LLVMBuildCondBr(codegen->builder, found, found_block, fail_block);

            LLVMPositionBuilderAtEnd(codegen->builder, fail_block);
            LLVMBuildStore(codegen->builder, bf_const_i32(codegen, 2),
                           codegen->oob_reason_ptr);
            LLVMBuildBr(codegen->builder, bf_get_oob_block(codegen));

            LLVMPositionBuilderAtEnd(codegen->builder, found_block);
            result_int = LLVMBuildPtrToInt(codegen->builder, result,
                                           codegen->i64_type, "scan_result_i");
            tape_int = LLVMBuildPtrToInt(codegen->builder, codegen->tape_arg,
                                         codegen->i64_type, "scan_tape_i");
            new_index = LLVMBuildSub(codegen->builder, result_int, tape_int,
                                     "scan_new_idx");
            LLVMBuildBr(codegen->builder, done_block);

            LLVMPositionBuilderAtEnd(codegen->builder, done_block);
            phi = LLVMBuildPhi(codegen->builder, codegen->i64_type,
                               "scan_ptr.phi");
            LLVMAddIncoming(phi, &pre_ptr, &pre_block, 1);
            LLVMAddIncoming(phi, &new_index, &found_block, 1);
            codegen->pointer_index = phi;
        } else if (node->arg == 4) {
            LLVMValueRef scan_args[3];
            LLVMValueRef result_idx;
            LLVMValueRef found;
            LLVMBasicBlockRef found_block;
            LLVMBasicBlockRef fail_block;

            scan_args[0] = codegen->tape_arg;
            scan_args[1] = codegen->tape_size_arg;
            scan_args[2] = codegen->pointer_index;
            result_idx = LLVMBuildCall2(
                codegen->builder,
                LLVMGlobalGetValueType(codegen->scan_index_step4_func),
                codegen->scan_index_step4_func, scan_args, 3, "scan_idx4");
            found = LLVMBuildICmp(codegen->builder, LLVMIntULT, result_idx,
                                  codegen->tape_size_arg, "scan_found4");
            found_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan4.found");
            fail_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan4.not_found");
            LLVMBuildCondBr(codegen->builder, found, found_block, fail_block);

            LLVMPositionBuilderAtEnd(codegen->builder, fail_block);
            LLVMBuildStore(codegen->builder, bf_const_i32(codegen, 3),
                           codegen->oob_reason_ptr);
            LLVMBuildBr(codegen->builder, bf_get_oob_block(codegen));

            LLVMPositionBuilderAtEnd(codegen->builder, found_block);
            codegen->pointer_index = result_idx;
        } else {
            LLVMValueRef scan_args[4];
            LLVMValueRef result_idx;
            LLVMValueRef found;
            LLVMBasicBlockRef found_block;
            LLVMBasicBlockRef fail_block;

            scan_args[0] = codegen->tape_arg;
            scan_args[1] = codegen->tape_size_arg;
            scan_args[2] = codegen->pointer_index;
            scan_args[3] = LLVMConstInt(
                codegen->i64_type, (unsigned long long)(int64_t)node->arg, 1);
            result_idx = LLVMBuildCall2(
                codegen->builder,
                LLVMGlobalGetValueType(codegen->scan_index_func),
                codegen->scan_index_func, scan_args, 4, "scan_idx");
            found = LLVMBuildICmp(codegen->builder, LLVMIntULT, result_idx,
                                  codegen->tape_size_arg, "scan_found");
            found_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.found");
            fail_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.not_found");
            LLVMBuildCondBr(codegen->builder, found, found_block, fail_block);

            LLVMPositionBuilderAtEnd(codegen->builder, fail_block);
            LLVMBuildStore(codegen->builder, bf_const_i32(codegen, 3),
                           codegen->oob_reason_ptr);
            LLVMBuildBr(codegen->builder, bf_get_oob_block(codegen));

            LLVMPositionBuilderAtEnd(codegen->builder, found_block);
            codegen->pointer_index = result_idx;
        }
        return 1;
    }
    case BF_IR_MULTIPLY_LOOP: {
        LLVMValueRef base_idx;
        LLVMValueRef loop_val;
        LLVMValueRef loop_is_nonzero;
        LLVMBasicBlockRef body_block;
        LLVMBasicBlockRef exit_block;
        int min_offset;
        int max_offset;
        size_t ti;

        cell_ptr = bf_build_current_cell_ptr(codegen);
        base_idx = codegen->pointer_index;

        loop_val = LLVMBuildLoad2(codegen->builder, codegen->i8_type, cell_ptr,
                                  "mul_loop_val");
        loop_is_nonzero =
            LLVMBuildICmp(codegen->builder, LLVMIntNE, loop_val,
                          bf_const_i8(codegen, 0), "mul_loop_nonzero");
        body_block = LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func,
                                                   "mul.body");
        exit_block = LLVMAppendBasicBlockInContext(codegen->ctx, codegen->func,
                                                   "mul.exit");

        LLVMBuildCondBr(codegen->builder, loop_is_nonzero, body_block,
                        exit_block);

        LLVMPositionBuilderAtEnd(codegen->builder, body_block);

        if (codegen->suppress_ptr_guards == 0 && node->term_count > 0) {
            LLVMValueRef min_idx;
            LLVMValueRef max_idx;

            min_offset = node->terms[0].offset;
            max_offset = node->terms[0].offset;
            for (ti = 1; ti < node->term_count; ++ti) {
                if (node->terms[ti].offset < min_offset) {
                    min_offset = node->terms[ti].offset;
                }
                if (node->terms[ti].offset > max_offset) {
                    max_offset = node->terms[ti].offset;
                }
            }

            min_idx = LLVMBuildAdd(
                codegen->builder, base_idx,
                LLVMConstInt(codegen->i64_type,
                             (unsigned long long)(int64_t)min_offset, 1),
                "mul_min_idx");
            bf_build_bounds_guard(codegen, min_idx, "mul.min.in_bounds", 4);

            if (max_offset != min_offset) {
                max_idx = LLVMBuildAdd(
                    codegen->builder, base_idx,
                    LLVMConstInt(codegen->i64_type,
                                 (unsigned long long)(int64_t)max_offset, 1),
                    "mul_max_idx");
                bf_build_bounds_guard(codegen, max_idx, "mul.max.in_bounds", 5);
            }
        }

        for (ti = 0; ti < node->term_count; ++ti) {
            LLVMValueRef target_idx;
            LLVMValueRef target_ptr;
            LLVMValueRef target_indices[1];
            LLVMValueRef old_val;
            LLVMValueRef product;
            LLVMValueRef new_val;

            target_idx = LLVMBuildAdd(
                codegen->builder, base_idx,
                LLVMConstInt(
                    codegen->i64_type,
                    (unsigned long long)(int64_t)node->terms[ti].offset, 1),
                "mul_target_idx");
            target_indices[0] = target_idx;
            target_ptr = LLVMBuildGEP2(codegen->builder, codegen->i8_type,
                                       codegen->tape_arg, target_indices, 1,
                                       "mul_target_ptr");
            old_val = LLVMBuildLoad2(codegen->builder, codegen->i8_type,
                                     target_ptr, "mul_old");
            product = LLVMBuildMul(codegen->builder, loop_val,
                                   bf_const_i8(codegen, node->terms[ti].factor),
                                   "mul_product");
            new_val =
                LLVMBuildAdd(codegen->builder, old_val, product, "mul_new");
            LLVMBuildStore(codegen->builder, new_val, target_ptr);
        }

        LLVMBuildStore(codegen->builder, bf_const_i8(codegen, 0), cell_ptr);
        LLVMBuildBr(codegen->builder, exit_block);

        LLVMPositionBuilderAtEnd(codegen->builder, exit_block);
        return 1;
    }
    default:
        bf_set_jit_err(codegen->err,
                       "unsupported IR node while generating LLVM IR");
        return 0;
    }
}

static int bf_codegen_block(bf_codegen *codegen, const bf_ir_block *block) {
    size_t index;

    for (index = 0; index < block->count;) {
        if (codegen->suppress_ptr_guards == 0) {
            bf_block_bounds guarded_bounds;
            size_t guarded_end;

            if (bf_find_guarded_range(block, index, &guarded_end,
                                      &guarded_bounds)) {
                bf_build_relative_bounds_guard(codegen, codegen->pointer_index,
                                               guarded_bounds.min_offset,
                                               guarded_bounds.max_offset, 8, 9);
                codegen->suppress_ptr_guards += 1;
                for (; index < guarded_end; ++index) {
                    if (!bf_codegen_node(codegen, &block->nodes[index])) {
                        codegen->suppress_ptr_guards -= 1;
                        return 0;
                    }
                }
                codegen->suppress_ptr_guards -= 1;
                continue;
            }
        }

        if (!bf_codegen_node(codegen, &block->nodes[index])) {
            return 0;
        }

        index += 1;
    }

    return 1;
}

LLVMModuleRef bf_build_module(LLVMContextRef ctx, const bf_program *program,
                              bf_jit_err *err) {
    bf_codegen codegen;
    LLVMTypeRef entry_args[2];
    LLVMTypeRef entry_type;
    LLVMTypeRef libc_args[1];
    LLVMBasicBlockRef entry_block;
    char *target_triple;
    char *verify_msg;
    LLVMModuleRef mod;
    LLVMValueRef return_value;

    memset(&codegen, 0, sizeof(codegen));
    codegen.ctx = ctx;
    codegen.mod = LLVMModuleCreateWithNameInContext("bfjit.mod", ctx);
    codegen.builder = LLVMCreateBuilderInContext(ctx);
    codegen.err = err;
    codegen.i1_type = LLVMInt1TypeInContext(ctx);
    codegen.i8_type = LLVMInt8TypeInContext(ctx);
    codegen.i32_type = LLVMInt32TypeInContext(ctx);
    codegen.i64_type = LLVMInt64TypeInContext(ctx);
    codegen.tape_pointer_type = LLVMPointerTypeInContext(ctx, 0);

    target_triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(codegen.mod, target_triple);
    LLVMDisposeMessage(target_triple);

    entry_args[0] = codegen.tape_pointer_type;
    entry_args[1] = codegen.i64_type;
    entry_type = LLVMFunctionType(codegen.i32_type, entry_args, 2, 0);
    codegen.func = LLVMAddFunction(codegen.mod, "bf_entry", entry_type);
    codegen.tape_arg = LLVMGetParam(codegen.func, 0);
    codegen.tape_size_arg = LLVMGetParam(codegen.func, 1);

    {
        unsigned noalias_kind = LLVMGetEnumAttributeKindForName("noalias", 7);
        unsigned nonnull_kind = LLVMGetEnumAttributeKindForName("nonnull", 7);
        unsigned nounwind_kind = LLVMGetEnumAttributeKindForName("nounwind", 8);

        LLVMAddAttributeAtIndex(codegen.func, 1,
                                LLVMCreateEnumAttribute(ctx, noalias_kind, 0));
        LLVMAddAttributeAtIndex(codegen.func, 1,
                                LLVMCreateEnumAttribute(ctx, nonnull_kind, 0));
        LLVMAddAttributeAtIndex(codegen.func, LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
    }

    libc_args[0] = codegen.i32_type;
    codegen.putchar_func =
        LLVMAddFunction(codegen.mod, "bf_io_putchar",
                        LLVMFunctionType(codegen.i32_type, libc_args, 1, 0));
    codegen.getchar_func =
        LLVMAddFunction(codegen.mod, "bf_io_getchar",
                        LLVMFunctionType(codegen.i32_type, NULL, 0, 0));

    {
        unsigned nounwind_kind = LLVMGetEnumAttributeKindForName("nounwind", 8);
        LLVMAddAttributeAtIndex(codegen.putchar_func,
                                LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
        LLVMAddAttributeAtIndex(codegen.getchar_func,
                                LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
    }

    {
        LLVMTypeRef memchr_params[3];
        LLVMTypeRef scan_params[4];
        LLVMTypeRef scan4_params[3];

        memchr_params[0] = codegen.tape_pointer_type;
        memchr_params[1] = codegen.i32_type;
        memchr_params[2] = codegen.i64_type;
        codegen.memchr_func = LLVMAddFunction(
            codegen.mod, "memchr",
            LLVMFunctionType(codegen.tape_pointer_type, memchr_params, 3, 0));
        bf_add_enum_attr_if_known(ctx, codegen.memchr_func,
                                  LLVMAttributeFunctionIndex, "nounwind", 8);

        scan_params[0] = codegen.tape_pointer_type;
        scan_params[1] = codegen.i64_type;
        scan_params[2] = codegen.i64_type;
        scan_params[3] = codegen.i64_type;
        codegen.scan_index_func = LLVMAddFunction(
            codegen.mod, "bf_io_scan_index",
            LLVMFunctionType(codegen.i64_type, scan_params, 4, 0));
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_func,
                                  LLVMAttributeFunctionIndex, "nounwind", 8);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_func,
                                  LLVMAttributeFunctionIndex, "willreturn", 10);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_func,
                                  LLVMAttributeFunctionIndex, "nofree", 6);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_func, 1, "nonnull",
                                  7);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_func, 1, "nocapture",
                                  9);

        scan4_params[0] = codegen.tape_pointer_type;
        scan4_params[1] = codegen.i64_type;
        scan4_params[2] = codegen.i64_type;
        codegen.scan_index_step4_func = LLVMAddFunction(
            codegen.mod, "bf_io_scan_index_step4",
            LLVMFunctionType(codegen.i64_type, scan4_params, 3, 0));
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_step4_func,
                                  LLVMAttributeFunctionIndex, "nounwind", 8);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_step4_func,
                                  LLVMAttributeFunctionIndex, "willreturn", 10);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_step4_func,
                                  LLVMAttributeFunctionIndex, "nofree", 6);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_step4_func, 1,
                                  "nonnull", 7);
        bf_add_enum_attr_if_known(ctx, codegen.scan_index_step4_func, 1,
                                  "nocapture", 9);
    }

    entry_block = LLVMAppendBasicBlockInContext(ctx, codegen.func, "entry");
    LLVMPositionBuilderAtEnd(codegen.builder, entry_block);
    codegen.oob_reason_ptr =
        LLVMBuildAlloca(codegen.builder, codegen.i32_type, "oob_reason");
    LLVMBuildStore(codegen.builder, bf_const_i32(&codegen, 0),
                   codegen.oob_reason_ptr);
    codegen.pointer_index = bf_const_i64(&codegen, 0);

    if (!bf_codegen_block(&codegen, &program->root)) {
        LLVMDisposeBuilder(codegen.builder);
        LLVMDisposeModule(codegen.mod);
        return NULL;
    }

    return_value = codegen.pointer_index;
    LLVMBuildRet(codegen.builder, LLVMBuildTrunc(codegen.builder, return_value,
                                                 codegen.i32_type, "result"));

    if (codegen.oob_block != NULL &&
        LLVMGetBasicBlockTerminator(codegen.oob_block) == NULL) {
        LLVMValueRef reason;

        LLVMPositionBuilderAtEnd(codegen.builder, codegen.oob_block);
        reason = LLVMBuildLoad2(codegen.builder, codegen.i32_type,
                                codegen.oob_reason_ptr, "oob_reason_val");
        LLVMBuildRet(codegen.builder,
                     LLVMBuildNeg(codegen.builder, reason, "oob_ret"));
    }

    if (LLVMVerifyModule(codegen.mod, LLVMReturnStatusAction, &verify_msg) !=
        0) {
        bf_set_jit_err(err, verify_msg);
        LLVMDisposeMessage(verify_msg);
        LLVMDisposeBuilder(codegen.builder);
        LLVMDisposeModule(codegen.mod);
        return NULL;
    }

    LLVMDisposeBuilder(codegen.builder);
    mod = codegen.mod;
    return mod;
}

int bf_opt_llvm_module(LLVMModuleRef mod, bf_jit_err *err) {
    LLVMTargetRef target;
    LLVMTargetMachineRef target_machine;
    LLVMPassBuilderOptionsRef pass_options;
    LLVMErrorRef llvm_err;
    char *triple;
    char *cpu;
    char *features;
    char *err_msg;

    triple = LLVMGetDefaultTargetTriple();

    if (LLVMGetTargetFromTriple(triple, &target, &err_msg) != 0) {
        bf_set_jit_err(err,
                       err_msg != NULL ? err_msg : "failed to resolve target");
        LLVMDisposeMessage(err_msg);
        LLVMDisposeMessage(triple);
        return 0;
    }

    cpu = LLVMGetHostCPUName();
    features = LLVMGetHostCPUFeatures();

    target_machine = LLVMCreateTargetMachine(
        target, triple, cpu, features, LLVMCodeGenLevelAggressive,
        LLVMRelocDefault, LLVMCodeModelDefault);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);

    if (target_machine == NULL) {
        bf_set_jit_err(err, "failed to create target machine");
        return 0;
    }

    pass_options = LLVMCreatePassBuilderOptions();
    llvm_err = LLVMRunPasses(mod,
                             "function(instcombine<no-verify-fixpoint>,"
                             "simplifycfg,gvn)",
                             target_machine, pass_options);
    LLVMDisposePassBuilderOptions(pass_options);
    LLVMDisposeTargetMachine(target_machine);

    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        return 0;
    }

    return 1;
}
