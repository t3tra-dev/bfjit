#define _GNU_SOURCE

#include "bfjit_internal.h"

#include <stdint.h>
#include <stdio.h>

static int bf_io_putchar(int c) { return putchar_unlocked(c); }

static int bf_io_getchar(void) { return getchar_unlocked(); }

static size_t bf_io_scan_index(const uint8_t *tape, size_t tape_size,
                               size_t start_index, int64_t step) {
    int64_t index;
    int64_t limit;

    if (tape == NULL || step == 0 || start_index >= tape_size) {
        return tape_size;
    }

    index = (int64_t)start_index;
    limit = (int64_t)tape_size;

    while (index >= 0 && index < limit) {
        if (tape[index] == 0) {
            return (size_t)index;
        }
        index += step;
    }

    return tape_size;
}

static size_t bf_io_scan_index_step4(const uint8_t *tape, size_t tape_size,
                                     size_t start_index) {
    size_t index;

    if (tape == NULL || start_index >= tape_size) {
        return tape_size;
    }

    index = start_index;

    while (index + 12 < tape_size) {
        if (tape[index] == 0) {
            return index;
        }
        if (tape[index + 4] == 0) {
            return index + 4;
        }
        if (tape[index + 8] == 0) {
            return index + 8;
        }
        if (tape[index + 12] == 0) {
            return index + 12;
        }
        index += 16;
    }

    while (index < tape_size) {
        if (tape[index] == 0) {
            return index;
        }
        if (index + 4 < tape_size) {
            index += 4;
        } else {
            break;
        }
    }

    return tape_size;
}

LLVMOrcMaterializationUnitRef bf_create_io_symbols(LLVMOrcLLJITRef jit) {
    LLVMOrcCSymbolMapPair io_symbols[4];

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

    io_symbols[2].Name = LLVMOrcLLJITMangleAndIntern(jit, "bf_io_scan_index");
    io_symbols[2].Sym.Address =
        (LLVMOrcExecutorAddress)(uintptr_t)bf_io_scan_index;
    io_symbols[2].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    io_symbols[2].Sym.Flags.TargetFlags = 0;

    io_symbols[3].Name =
        LLVMOrcLLJITMangleAndIntern(jit, "bf_io_scan_index_step4");
    io_symbols[3].Sym.Address =
        (LLVMOrcExecutorAddress)(uintptr_t)bf_io_scan_index_step4;
    io_symbols[3].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    io_symbols[3].Sym.Flags.TargetFlags = 0;

    return LLVMOrcAbsoluteSymbols(io_symbols, 4);
}
