#define _GNU_SOURCE

#include "bfjit_internal.h"

#include <stdio.h>

#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <stdint.h>

typedef int (*bf_jit_entry_fn)(uint8_t *tape, size_t tape_size);

void bf_jit_err_reset(bf_jit_err *err) {
    if (err == NULL) {
        return;
    }

    err->has_err = false;
    err->msg[0] = '\0';
}

void bf_set_jit_err(bf_jit_err *err, const char *msg) {
    if (err == NULL) {
        return;
    }

    err->has_err = true;
    snprintf(err->msg, sizeof(err->msg), "%s", msg);
}

void bf_set_jit_err_from_llvm(bf_jit_err *err, LLVMErrorRef llvm_err) {
    char *msg;

    if (llvm_err == NULL) {
        bf_set_jit_err(err, "unknown LLVM err");
        return;
    }

    msg = LLVMGetErrorMessage(llvm_err);
    bf_set_jit_err(err, msg != NULL ? msg : "unknown LLVM err");
    LLVMDisposeErrorMessage(msg);
}

bool bf_jit_execute_program(const bf_program *program, uint8_t *tape,
                            size_t tape_size, bf_jit_err *err) {
    LLVMOrcLLJITRef jit;
    LLVMOrcThreadSafeContextRef thread_safe_ctx;
    LLVMOrcThreadSafeModuleRef thread_safe_mod;
    LLVMOrcDefinitionGeneratorRef generator;
    LLVMOrcMaterializationUnitRef io_mu;
    LLVMErrorRef llvm_err;
    LLVMOrcExecutorAddress entry_address;
    LLVMModuleRef mod;
    LLVMContextRef ctx;
    bf_jit_entry_fn entry_func;
    int execution_result;

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

    io_mu = bf_create_io_symbols(jit);
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
    mod = bf_build_module(ctx, program, err);
    if (mod == NULL) {
        LLVMOrcDisposeThreadSafeContext(thread_safe_ctx);
        LLVMOrcDisposeLLJIT(jit);
        return false;
    }

    LLVMSetDataLayout(mod, LLVMOrcLLJITGetDataLayoutStr(jit));

    if (!bf_opt_llvm_module(mod, err)) {
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
    execution_result = entry_func(tape, tape_size);
    funlockfile(stdin);
    funlockfile(stdout);

    llvm_err = LLVMOrcDisposeLLJIT(jit);
    if (llvm_err != NULL) {
        bf_set_jit_err_from_llvm(err, llvm_err);
        return false;
    }

    if (execution_result < 0) {
        switch (-execution_result) {
        case 1:
            bf_set_jit_err(err, "tape pointer moved out of bounds (add_ptr)");
            break;
        case 2:
            bf_set_jit_err(err,
                           "tape pointer moved out of bounds (scan memchr)");
            break;
        case 3:
            bf_set_jit_err(err, "tape pointer moved out of bounds (scan)");
            break;
        case 4:
            bf_set_jit_err(err,
                           "tape pointer moved out of bounds (multiply min)");
            break;
        case 5:
            bf_set_jit_err(err,
                           "tape pointer moved out of bounds (multiply max)");
            break;
        case 6:
            bf_set_jit_err(err, "tape pointer moved out of bounds (loop min)");
            break;
        case 7:
            bf_set_jit_err(err, "tape pointer moved out of bounds (loop max)");
            break;
        case 8:
            bf_set_jit_err(err,
                           "tape pointer moved out of bounds (segment min)");
            break;
        case 9:
            bf_set_jit_err(err,
                           "tape pointer moved out of bounds (segment max)");
            break;
        default:
            bf_set_jit_err(err, "tape pointer moved out of bounds");
            break;
        }
        return false;
    }

    return true;
}
