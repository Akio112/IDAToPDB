
#pragma once

#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// ID типа внутри одного вызова generateTPI.
// Значение 0 зарезервировано и не является допустимым ID.
using TypeId = uint32_t;

enum class TypeKind : uint8_t {
    Primitive = 0,
    Pointer = 1,
    LValueReference = 2,
    RValueReference = 3,
    Array = 4
};

enum class TypeQualifier : uint16_t {
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

// Описание одного типа.
//
// Вложенные типы задаются через elementTypeId,
// а не через shared_ptr или вложенный TypeRequest.
struct TypeRequest {
    TypeId id = 0;

    TypeKind kind = TypeKind::Primitive;

    // Используется только для Primitive.
    llvm::codeview::SimpleTypeKind base =
        llvm::codeview::SimpleTypeKind::None;

    TypeQualifiers qualifiers = 0;

    // ID типа, на который ссылается указатель/ссылка,
    // или ID типа элемента массива.
    // Для Primitive должен быть равен 0.
    TypeId elementTypeId = 0;

    // Полный размер массива в байтах, а не одного элемента.
    uint64_t sizeBytes = 0;
};

// Описание сигнатуры функции.
//
// Собственный ID не нужен: результаты возвращаются
// в том же порядке, что и входные сигнатуры.
struct FunctionSignatureRequest {
    TypeId returnTypeId = 0;

    std::vector<TypeId> argTypeIds;

    llvm::codeview::CallingConvention callingConvention =
        llvm::codeview::CallingConvention::NearC;

    bool variadic = false;
};

// Результат генерации TPI.
struct TPITypes {
    // ID входного типа -> настоящий CodeView TypeIndex.
    std::unordered_map<
        TypeId,
        llvm::codeview::TypeIndex
    > typeIndices;

    // TypeIndex сигнатур в порядке RequestedFunctions.
    std::vector<llvm::codeview::TypeIndex> functionTypes;
};

// Параметры анализируемого бинарника.
struct TPITarget {
    // 4 для 32-битного, 8 для 64-битного бинарника.
    uint8_t pointerSize = 8;
};

llvm::Expected<TPITypes> generateTPI(
    llvm::pdb::PDBFileBuilder &PdbBuilder,
    llvm::BumpPtrAllocator &Allocator,
    const TPITarget &Target,
    const std::vector<TypeRequest> &RequestedTypes,
    const std::vector<FunctionSignatureRequest> &RequestedFunctions
);
