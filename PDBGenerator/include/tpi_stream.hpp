// ===== Поток TPI (типы) =====
// Часть Анны: типы функций int(int, int), int(const char*), int().
// Позже (когда будут символы функций внутри модуля) на эти типы будут ссылаться по номеру.
#pragma once

#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"

// Номера созданных сигнатур в таблице TPI (номера начинаются с 0x1000).
struct FunctionTypes {
    llvm::codeview::TypeIndex FuncSigType;          // int(int, int)      my_add, my_multiply
    llvm::codeview::TypeIndex CheckPwdFuncSigType;  // int(const char*)   my_check_password
    llvm::codeview::TypeIndex MainFuncSigType;      // int()              main
};

// Allocator должен жить до PdbBuilder.commit(): в нём лежат байты записей типов.
llvm::Expected<FunctionTypes> generateTPI(llvm::pdb::PDBFileBuilder &PdbBuilder,
                                          llvm::BumpPtrAllocator &Allocator);
