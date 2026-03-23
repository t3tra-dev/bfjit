#define _GNU_SOURCE

#include "bfjit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

typedef int (*bf_jit_entry_fn)(uint8_t *tape, size_t tape_size);

static int bf_io_putchar(int c) { return putchar_unlocked(c); }

static int bf_io_getchar(void) { return getchar_unlocked(); }

typedef struct bf_codegen {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMValueRef func;
    LLVMValueRef tape_arg;
    LLVMValueRef tape_size_arg;
    LLVMValueRef pointer_index;
    LLVMValueRef memchr_func;
    LLVMTypeRef i1_type;
    LLVMTypeRef i8_type;
    LLVMTypeRef i32_type;
    LLVMTypeRef i64_type;
    LLVMTypeRef tape_pointer_type;
    LLVMValueRef putchar_func;
    LLVMValueRef getchar_func;
    bf_jit_err *err;
} bf_codegen;

static void bf_jit_err_reset(bf_jit_err *err) {
    if (err == NULL) {
        return;
    }

    err->has_err = false;
    err->msg[0] = '\0';
}

static void bf_set_jit_err(bf_jit_err *err, const char *msg) {
    if (err == NULL) {
        return;
    }

    err->has_err = true;
    snprintf(err->msg, sizeof(err->msg), "%s", msg);
}

static void bf_set_jit_err_from_llvm(bf_jit_err *err, LLVMErrorRef llvm_err) {
    char *msg;

    if (llvm_err == NULL) {
        bf_set_jit_err(err, "unknown LLVM err");
        return;
    }

    msg = LLVMGetErrorMessage(llvm_err);
    bf_set_jit_err(err, msg != NULL ? msg : "unknown LLVM err");
    LLVMDisposeErrorMessage(msg);
}

static LLVMValueRef bf_const_i8(bf_codegen *codegen, int value) {
    return LLVMConstInt(codegen->i8_type, (unsigned long long)(uint8_t)value,
                        0);
}

static LLVMValueRef bf_const_i64(bf_codegen *codegen, uint64_t value) {
    return LLVMConstInt(codegen->i64_type, value, 0);
}

static LLVMValueRef bf_build_current_cell_ptr(bf_codegen *codegen) {
    LLVMValueRef indices[1];

    indices[0] = codegen->pointer_index;

    return LLVMBuildGEP2(codegen->builder, codegen->i8_type, codegen->tape_arg,
                         indices, 1, "cell_ptr");
}

static int bf_codegen_block(bf_codegen *codegen, const bf_ir_block *block);

static int bf_codegen_loop(bf_codegen *codegen, const bf_ir_node *node) {
    LLVMBasicBlockRef pre_loop_block;
    LLVMValueRef pre_loop_ptr;
    LLVMBasicBlockRef condition_block;
    LLVMBasicBlockRef body_block;
    LLVMBasicBlockRef exit_block;
    LLVMValueRef ptr_phi;
    LLVMValueRef cell_ptr;
    LLVMValueRef cell_value;
    LLVMValueRef loop_condition;
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

    LLVMBuildBr(codegen->builder, condition_block);

    LLVMPositionBuilderAtEnd(codegen->builder, condition_block);
    ptr_phi = LLVMBuildPhi(codegen->builder, codegen->i64_type, "ptr.phi");
    LLVMAddIncoming(ptr_phi, &pre_loop_ptr, &pre_loop_block, 1);
    codegen->pointer_index = ptr_phi;

    cell_ptr = bf_build_current_cell_ptr(codegen);
    cell_value = LLVMBuildLoad2(codegen->builder, codegen->i8_type, cell_ptr,
                                "loop_value");
    loop_condition = LLVMBuildICmp(codegen->builder, LLVMIntNE, cell_value,
                                   bf_const_i8(codegen, 0), "loop_condition");
    LLVMBuildCondBr(codegen->builder, loop_condition, body_block, exit_block);

    LLVMPositionBuilderAtEnd(codegen->builder, body_block);
    if (!bf_codegen_block(codegen, &node->body)) {
        return 0;
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
    case BF_IR_SCAN: {
        if (node->arg == 1) {
            /* [>] の memchr 最適化パス */
            LLVMValueRef search_ptr;
            LLVMValueRef remaining;
            LLVMValueRef call_args[3];
            LLVMValueRef result;
            LLVMValueRef found;
            LLVMValueRef result_int;
            LLVMValueRef tape_int;
            LLVMValueRef new_index;
            LLVMValueRef phi;
            LLVMBasicBlockRef pre_block;
            LLVMBasicBlockRef found_block;
            LLVMBasicBlockRef done_block;
            LLVMValueRef pre_ptr;

            pre_block = LLVMGetInsertBlock(codegen->builder);
            pre_ptr = codegen->pointer_index;

            search_ptr = bf_build_current_cell_ptr(codegen);
            remaining = LLVMBuildSub(codegen->builder, codegen->tape_size_arg,
                                     codegen->pointer_index, "scan_remaining");

            call_args[0] = search_ptr;
            call_args[1] = LLVMConstInt(codegen->i32_type, 0, 0);
            call_args[2] = remaining;
            result = LLVMBuildCall2(
                codegen->builder, LLVMGlobalGetValueType(codegen->memchr_func),
                codegen->memchr_func, call_args, 3, "scan_memchr");

            found = LLVMBuildICmp(codegen->builder, LLVMIntNE, result,
                                  LLVMConstNull(codegen->tape_pointer_type),
                                  "scan_found");

            found_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.found");
            done_block = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.done");

            LLVMBuildCondBr(codegen->builder, found, found_block, done_block);

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
            LLVMAddIncoming(phi, &new_index, &found_block, 1);
            LLVMAddIncoming(phi, &pre_ptr, &pre_block, 1);

            codegen->pointer_index = phi;
        } else {
            /* PHI 命令による一般スキャンループ */
            LLVMBasicBlockRef pre_block;
            LLVMValueRef pre_ptr;
            LLVMBasicBlockRef scan_cond;
            LLVMBasicBlockRef scan_step;
            LLVMBasicBlockRef scan_exit;
            LLVMValueRef scan_phi;
            LLVMValueRef scan_val;
            LLVMValueRef scan_cmp;
            LLVMValueRef next_idx;

            pre_block = LLVMGetInsertBlock(codegen->builder);
            pre_ptr = codegen->pointer_index;

            scan_cond = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.cond");
            scan_step = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.step");
            scan_exit = LLVMAppendBasicBlockInContext(
                codegen->ctx, codegen->func, "scan.exit");

            LLVMBuildBr(codegen->builder, scan_cond);

            LLVMPositionBuilderAtEnd(codegen->builder, scan_cond);
            scan_phi = LLVMBuildPhi(codegen->builder, codegen->i64_type,
                                    "scan_ptr.phi");
            LLVMAddIncoming(scan_phi, &pre_ptr, &pre_block, 1);
            codegen->pointer_index = scan_phi;

            cell_ptr = bf_build_current_cell_ptr(codegen);
            scan_val = LLVMBuildLoad2(codegen->builder, codegen->i8_type,
                                      cell_ptr, "scan_val");
            scan_cmp = LLVMBuildICmp(codegen->builder, LLVMIntNE, scan_val,
                                     bf_const_i8(codegen, 0), "scan_cmp");
            LLVMBuildCondBr(codegen->builder, scan_cmp, scan_step, scan_exit);

            LLVMPositionBuilderAtEnd(codegen->builder, scan_step);
            next_idx = LLVMBuildAdd(
                codegen->builder, scan_phi,
                LLVMConstInt(codegen->i64_type,
                             (unsigned long long)(int64_t)node->arg, 1),
                "scan_next");
            LLVMAddIncoming(scan_phi, &next_idx, &scan_step, 1);
            LLVMBuildBr(codegen->builder, scan_cond);

            LLVMPositionBuilderAtEnd(codegen->builder, scan_exit);
            codegen->pointer_index = scan_phi;
        }
        return 1;
    }
    case BF_IR_MULTIPLY_LOOP: {
        LLVMValueRef base_idx;
        LLVMValueRef loop_val;
        size_t ti;

        cell_ptr = bf_build_current_cell_ptr(codegen);
        loop_val = LLVMBuildLoad2(codegen->builder, codegen->i8_type, cell_ptr,
                                  "mul_loop_val");
        base_idx = codegen->pointer_index;

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

        /* ループ変数セルをクリア */
        LLVMBuildStore(codegen->builder, bf_const_i8(codegen, 0), cell_ptr);
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

    for (index = 0; index < block->count; ++index) {
        if (!bf_codegen_node(codegen, &block->nodes[index])) {
            return 0;
        }
    }

    return 1;
}

static LLVMModuleRef bf_build_mod(LLVMContextRef ctx, const bf_program *program,
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

        /* tape pointer: noalias nonnull */
        LLVMAddAttributeAtIndex(codegen.func, 1,
                                LLVMCreateEnumAttribute(ctx, noalias_kind, 0));
        LLVMAddAttributeAtIndex(codegen.func, 1,
                                LLVMCreateEnumAttribute(ctx, nonnull_kind, 0));

        /* bf_entry: nounwind */
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

    /* putchar/getchar: nounwind */
    {
        unsigned nounwind_kind = LLVMGetEnumAttributeKindForName("nounwind", 8);
        LLVMAddAttributeAtIndex(codegen.putchar_func,
                                LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
        LLVMAddAttributeAtIndex(codegen.getchar_func,
                                LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
    }

    /* memchr: ptr @memchr(ptr, i32, i64) nounwind */
    {
        LLVMTypeRef memchr_params[3];
        unsigned nounwind_kind = LLVMGetEnumAttributeKindForName("nounwind", 8);

        memchr_params[0] = codegen.tape_pointer_type;
        memchr_params[1] = codegen.i32_type;
        memchr_params[2] = codegen.i64_type;
        codegen.memchr_func = LLVMAddFunction(
            codegen.mod, "memchr",
            LLVMFunctionType(codegen.tape_pointer_type, memchr_params, 3, 0));
        LLVMAddAttributeAtIndex(codegen.memchr_func, LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx, nounwind_kind, 0));
    }

    entry_block = LLVMAppendBasicBlockInContext(ctx, codegen.func, "entry");
    LLVMPositionBuilderAtEnd(codegen.builder, entry_block);
    codegen.pointer_index = bf_const_i64(&codegen, 0);

    if (!bf_codegen_block(&codegen, &program->root)) {
        LLVMDisposeBuilder(codegen.builder);
        LLVMDisposeModule(codegen.mod);
        return NULL;
    }

    return_value = codegen.pointer_index;
    LLVMBuildRet(codegen.builder, LLVMBuildTrunc(codegen.builder, return_value,
                                                 codegen.i32_type, "result"));

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

static int bf_opt_mod(LLVMModuleRef mod, bf_jit_err *err) {
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

bool bf_jit_execute_program(const bf_program *program, uint8_t *tape,
                            size_t tape_size, bf_jit_err *err) {
    LLVMOrcLLJITRef jit;
    LLVMOrcThreadSafeContextRef thread_safe_ctx;
    LLVMOrcThreadSafeModuleRef thread_safe_mod;
    LLVMOrcDefinitionGeneratorRef generator;
    LLVMOrcMaterializationUnitRef io_mu;
    LLVMOrcCSymbolMapPair io_symbols[2];
    LLVMErrorRef llvm_err;
    LLVMOrcExecutorAddress entry_address;
    LLVMModuleRef mod;
    LLVMContextRef ctx;
    bf_jit_entry_fn entry_func;

    bf_jit_err_reset(err);

    if (program == NULL || tape == NULL) {
        bf_set_jit_err(err, "program and tape must be non-null");
        return false;
    }

    if (LLVMInitializeNativeTarget() != 0 ||
        LLVMInitializeNativeAsmPrinter() != 0 ||
        LLVMInitializeNativeAsmParser() != 0) {
        bf_set_jit_err(err, "failed to initialize LLVM native target support");
        return false;
    }

    llvm_err = LLVMOrcCreateLLJIT(&jit, NULL);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        return false;
    }

    /* unlock された I/O ラッパーを絶対シンボルとして登録 (名前修飾に対応) */
    io_symbols[0].Name = LLVMOrcLLJITMangleAndIntern(jit, "bf_io_putchar");
    io_symbols[0].Sym.Address =
        (LLVMOrcExecutorAddress)(uintptr_t)bf_io_putchar;
    io_symbols[0].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    io_symbols[0].Sym.Flags.TargetFlags = 0;

    io_symbols[1].Name = LLVMOrcLLJITMangleAndIntern(jit, "bf_io_getchar");
    io_symbols[1].Sym.Address =
        (LLVMOrcExecutorAddress)(uintptr_t)bf_io_getchar;
    io_symbols[1].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    io_symbols[1].Sym.Flags.TargetFlags = 0;

    io_mu = LLVMOrcAbsoluteSymbols(io_symbols, 2);
    llvm_err = LLVMOrcJITDylibDefine(LLVMOrcLLJITGetMainJITDylib(jit), io_mu);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    llvm_err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
        &generator, LLVMOrcLLJITGetGlobalPrefix(jit), NULL, NULL);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(jit), generator);

    ctx = LLVMContextCreate();
    thread_safe_ctx = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(ctx);
    mod = bf_build_mod(ctx, program, err);
    if (mod == NULL) {
        LLVMOrcDisposeThreadSafeContext(thread_safe_ctx);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    LLVMSetDataLayout(mod, LLVMOrcLLJITGetDataLayoutStr(jit));

    if (!bf_opt_mod(mod, err)) {
        LLVMDisposeModule(mod);
        LLVMOrcDisposeThreadSafeContext(thread_safe_ctx);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    thread_safe_mod = LLVMOrcCreateNewThreadSafeModule(mod, thread_safe_ctx);

    llvm_err = LLVMOrcLLJITAddLLVMIRModule(
        jit, LLVMOrcLLJITGetMainJITDylib(jit), thread_safe_mod);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        LLVMOrcDisposeThreadSafeModule(thread_safe_mod);
        LLVMOrcDisposeThreadSafeContext(thread_safe_ctx);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    llvm_err = LLVMOrcLLJITLookup(jit, &entry_address, "bf_entry");
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    entry_func = (bf_jit_entry_fn)(uintptr_t)entry_address;

    flockfile(stdout);
    flockfile(stdin);
    (void)entry_func(tape, tape_size);
    funlockfile(stdin);
    funlockfile(stdout);

    llvm_err = LLVMOrcDisposeLLJIT(jit);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        return false;
    }

    return true;
}
