#ifndef BFJIT_INTERNAL_H
#define BFJIT_INTERNAL_H

#include "bfjit.h"

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>

void bf_jit_err_reset(bf_jit_err *err);
void bf_set_jit_err(bf_jit_err *err, const char *msg);
void bf_set_jit_err_from_llvm(bf_jit_err *err, LLVMErrorRef llvm_err);

LLVMModuleRef bf_build_module(LLVMContextRef ctx, const bf_program *program,
                              bf_jit_err *err);
int bf_opt_llvm_module(LLVMModuleRef mod, bf_jit_err *err);

LLVMOrcMaterializationUnitRef bf_create_io_symbols(LLVMOrcLLJITRef jit);

#endif
