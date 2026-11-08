#pragma once

#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <memory>

enum class TypeKind {
    Primitive,
    Pointer,
    LValueReference, // int&
    RValueReference, //int&&
    Array
};

enum class TypeQualifier : uint16_t { // unsigned, 2 bytes
    None      = 0,
    Const     = 1 << 0,
    Volatile  = 1 << 1,
    Unaligned = 1 << 2,
    Restrict  = 1 << 3
};
using TypeQualifiers = uint16_t;

constexpr TypeQualifiers operator|(
    TypeQualifier lhs,
    TypeQualifier rhs) {

    return static_cast<TypeQualifiers>(lhs) |
           static_cast<TypeQualifiers>(rhs);
}
constexpr TypeQualifiers operator|(
    TypeQualifiers lhs,
    TypeQualifier rhs) {

    return lhs | static_cast<TypeQualifiers>(rhs);
}

constexpr bool hasQualifier(
    TypeQualifiers flags,
    TypeQualifier qualifier) {

    return (flags & static_cast<TypeQualifiers>(qualifier)) != 0;
}
struct TypeRequest {
    TypeKind kind = TypeKind::Primitive;

    // Базовый тип (Int32, NarrowCharacter, Float32 и т.д.)
    llvm::codeview::SimpleTypeKind base = llvm::codeview::SimpleTypeKind::None;

    TypeQualifiers qualifiers = 0; 
    // вложенный тип int**
    std::shared_ptr<const TypeRequest> elementType;
    // PDB может генерироваться для 32-битного бинарника
    uint8_t pointerSize = 0;
    // количество элементов массива
    uint64_t arrayCount = 0;
    // всего массива
    uint64_t sizeBytes = 0;
};
struct FunctionSignatureRequest {
    TypeRequest returnType;                 
    std::vector<TypeRequest> argTypes;
    
    llvm::codeview::CallingConvention callingConvention = llvm::codeview::CallingConvention::NearC;

    bool variadic = false;
};
struct TPITypes {
    std::vector<llvm::codeview::TypeIndex> SimpleTypes;
    std::vector<llvm::codeview::TypeIndex> ComplexTypes;
    std::vector<llvm::codeview::TypeIndex> FunctionTypes;
};
// описание бинарника, который анализирует IDA
struct TPITarget {
    uint8_t pointerSize = 8;
};

llvm::Expected<TPITypes> generateTPI(
    llvm::pdb::PDBFileBuilder &PdbBuilder,
    llvm::BumpPtrAllocator &Allocator,
    const TPITarget &Target,
    const std::vector<llvm::codeview::SimpleTypeKind>
        &RequestedSimpleTypes,
    const std::vector<TypeRequest> &RequestedComplexTypes,
    const std::vector<FunctionSignatureRequest>
        &RequestedFunctions);


