// ===== Поток TPI (типы) =====
// Часть Анны: типы функций int(int, int), int(const char*), int().
// Позже (когда будут символы функций внутри модуля) на эти типы будут ссылаться по номеру.
#pragma once

#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include <map>
#include <vector>

struct TypeRequest {
    // Базовый тип (Int32, NarrowCharacter, Float32 и т.д.)
    llvm::codeview::SimpleTypeKind base;

    // Модификаторы
    bool isConst = false;
    bool isPointer = false;

    // Оператор сравнения нужен для std::map.
    // Сравниваем по всем полям, чтобы разные описания не схлопнулись в один ключ.
    bool operator<(const TypeRequest &other) const {
        if (base != other.base) return base < other.base;
        if (isConst != other.isConst) return isConst < other.isConst;
        return isPointer < other.isPointer;
    }
};
struct FunctionSignatureRequest {
    TypeRequest returnType;                 
    std::vector<TypeRequest> argTypes;
    
    // Оператор сравнения для std::map
    bool operator<(const FunctionSignatureRequest &other) const {
        if (returnType < other.returnType) return true;
        if (other.returnType < returnType) return false;
        return argTypes < other.argTypes;
    }
};
struct TPITypes {
    // Базовые типы (int, double)
    std::map<SimpleTypeKind, TypeIndex> SimpleTypes;

    // Модифицированные типы (int*, const char, const double*)
    std::map<TypeRequest, TypeIndex> ModifiedTypes;

// Словарь сигнатур функций (ключ — описание, значение — TypeIndex)
    std::map<FunctionSignatureRequest, llvm::codeview::TypeIndex> FunctionTypes;

};

// Allocator должен жить до PdbBuilder.commit(): в нём лежат байты записей типов.
llvm::Expected<TPITypes> generateTPI(
    llvm::pdb::PDBFileBuilder &PdbBuilder,
    llvm::BumpPtrAllocator &Allocator,
    const std::vector<llvm::codeview::SimpleTypeKind> &RequestedSimpleTypes,
    const std::vector<TypeRequest> &RequestedComplexTypes,
    const std::vector<FunctionSignatureRequest> &RequestedFunctions);
                                          llvm::BumpPtrAllocator &Allocator,
                                        std::vector<llvm::codeview::SimpleTypeKind> &RequestedTypes);
