
#include "tpi_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/Error.h"

using namespace llvm;
using namespace llvm::codeview;

namespace {

constexpr unsigned MaxTypeDepth = 64;

constexpr TypeQualifiers KnownQualifiers =
    static_cast<TypeQualifiers>(TypeQualifier::Const) |
    static_cast<TypeQualifiers>(TypeQualifier::Volatile) |
    static_cast<TypeQualifiers>(TypeQualifier::Unaligned) |
    static_cast<TypeQualifiers>(TypeQualifier::Restrict);

static Error typeError(const char *message) {
    return createStringError(
        std::errc::invalid_argument,
        "%s",
        message
    );
}

static bool isReference(TypeKind kind) {
    return kind == TypeKind::LValueReference ||
           kind == TypeKind::RValueReference;
}

static bool isPointerLike(TypeKind kind) {
    return kind == TypeKind::Pointer || isReference(kind);
}

static bool isVoidType(const TypeRequest &req) {
    return req.kind == TypeKind::Primitive &&
           req.base == SimpleTypeKind::Void;
}

static bool hasUnknownQualifiers(TypeQualifiers qualifiers) {
    return (qualifiers & ~KnownQualifiers) != 0;
}

static uint8_t getPointerSize(
    const TypeRequest &req,
    const TPITarget &target) {

    if (req.pointerSize != 0)
        return req.pointerSize;

    return target.pointerSize;
}

static ModifierOptions getModifierOptions(
    TypeQualifiers qualifiers) {

    ModifierOptions options = ModifierOptions::None;

    if (hasQualifier(qualifiers, TypeQualifier::Const))
        options |= ModifierOptions::Const;

    if (hasQualifier(qualifiers, TypeQualifier::Volatile))
        options |= ModifierOptions::Volatile;

    if (hasQualifier(qualifiers, TypeQualifier::Unaligned))
        options |= ModifierOptions::Unaligned;

    return options;
}

static PointerOptions getPointerOptions(
    TypeQualifiers qualifiers) {

    PointerOptions options = PointerOptions::None;

    if (hasQualifier(qualifiers, TypeQualifier::Const))
        options |= PointerOptions::Const;

    if (hasQualifier(qualifiers, TypeQualifier::Volatile))
        options |= PointerOptions::Volatile;

    if (hasQualifier(qualifiers, TypeQualifier::Unaligned))
        options |= PointerOptions::Unaligned;

    if (hasQualifier(qualifiers, TypeQualifier::Restrict))
        options |= PointerOptions::Restrict;

    return options;
}

static Error validateType(
    const TypeRequest &req,
    const TPITarget &target,
    unsigned depth = 0) {

    if (depth >= MaxTypeDepth)
        return typeError("Type nesting is too deep");

    if (hasUnknownQualifiers(req.qualifiers))
        return typeError("Unknown type qualifier");

    switch (req.kind) {
    case TypeKind::Primitive: {
        if (req.base == SimpleTypeKind::None)
            return typeError("Primitive type is not specified");

        if (req.elementType)
            return typeError("Primitive cannot have elementType");

        if (req.pointerSize != 0 ||
            req.arrayCount != 0 ||
            req.sizeBytes != 0)
            return typeError("Primitive contains unrelated fields");

        if (hasQualifier(req.qualifiers, TypeQualifier::Restrict))
            return typeError("Restrict is only supported on pointers");

        return Error::success();
    }

    case TypeKind::Pointer:
    case TypeKind::LValueReference:
    case TypeKind::RValueReference: {
        if (!req.elementType)
            return typeError("Pointer or reference has no nested type");

        if (req.base != SimpleTypeKind::None)
            return typeError("Pointer or reference cannot specify base");

        if (req.arrayCount != 0 || req.sizeBytes != 0)
            return typeError("Pointer or reference has array fields");

        const uint8_t size = getPointerSize(req, target);

        if (size != 4 && size != 8)
            return typeError("Pointer size must be 4 or 8 bytes");

        if (isReference(req.kind)) {
            if (req.qualifiers != 0)
                return typeError(
                    "Reference cannot have top-level qualifiers"
                );

            if (isVoidType(*req.elementType))
                return typeError("Reference to void is invalid");
        }

        if (isReference(req.elementType->kind))
            return typeError(
                "Pointer or reference to reference is invalid"
            );

        return validateType(
            *req.elementType,
            target,
            depth + 1
        );
    }

    case TypeKind::Array: {
        if (!req.elementType)
            return typeError("Array has no element type");

        if (req.base != SimpleTypeKind::None)
            return typeError("Array cannot specify base");

        if (req.qualifiers != 0)
            return typeError(
                "Array qualifiers must be applied to its element"
            );

        if (req.pointerSize != 0)
            return typeError("Array cannot specify pointerSize");

        if (req.arrayCount == 0)
            return typeError("Array count must be positive");

        if (req.sizeBytes == 0)
            return typeError("Array sizeBytes must be positive");

        if (isReference(req.elementType->kind))
            return typeError("Array of references is invalid");

        if (isVoidType(*req.elementType))
            return typeError("Array of void is invalid");

        return validateType(
            *req.elementType,
            target,
            depth + 1
        );
    }
    }

    return typeError("Unsupported TypeKind");
}

static Error validateFunction(
    const FunctionSignatureRequest &func,
    const TPITarget &target) {

    if (func.returnType.kind == TypeKind::Array)
        return typeError("Function cannot return an array by value");

    if (Error err = validateType(func.returnType, target))
        return err;

    const size_t maxArgs =
        std::numeric_limits<uint16_t>::max();

    if (func.argTypes.size() > maxArgs)
        return typeError("Too many function arguments");

    if (func.variadic && func.argTypes.size() == maxArgs)
        return typeError("Too many variadic function arguments");

    for (const TypeRequest &arg : func.argTypes) {
        if (Error err = validateType(arg, target))
            return err;

        if (isVoidType(arg))
            return typeError(
                "Void cannot be used as an explicit argument"
            );
    }

    return Error::success();
}

static Expected<TypeIndex> createType(
    const TypeRequest &req,
    AppendingTypeTableBuilder &table,
    const TPITarget &target) {

    switch (req.kind) {
    case TypeKind::Primitive: {
        TypeIndex current(req.base);

        const ModifierOptions options =
            getModifierOptions(req.qualifiers);

        if (options != ModifierOptions::None) {
            ModifierRecord modifier(current, options);
            current = table.writeLeafType(modifier);
        }

        return current;
    }

    case TypeKind::Pointer:
    case TypeKind::LValueReference:
    case TypeKind::RValueReference: {
        auto nested = createType(
            *req.elementType,
            table,
            target
        );

        if (!nested)
            return nested.takeError();

        const uint8_t size = getPointerSize(req, target);

        const PointerKind pointerKind =
            size == 8
                ? PointerKind::Near64
                : PointerKind::Near32;

        PointerMode mode = PointerMode::Pointer;

        if (req.kind == TypeKind::LValueReference)
            mode = PointerMode::LValueReference;
        else if (req.kind == TypeKind::RValueReference)
            mode = PointerMode::RValueReference;

        PointerRecord pointer(
            *nested,
            pointerKind,
            mode,
            getPointerOptions(req.qualifiers),
            size
        );

        return table.writeLeafType(pointer);
    }

    case TypeKind::Array: {
        auto elementIndex = createType(
            *req.elementType,
            table,
            target
        );

        if (!elementIndex)
            return elementIndex.takeError();

        // LF_ARRAY хранит полный размер массива в байтах.
        // Для обычного массива индексным типом используем UInt32.
        ArrayRecord array(
            *elementIndex,
            TypeIndex(SimpleTypeKind::UInt32),
            req.sizeBytes,
            ""
        );

        return table.writeLeafType(array);
    }
    }

    return typeError("Unsupported TypeKind");
}

static Expected<TypeIndex> createFunctionSignature(
    const FunctionSignatureRequest &func,
    AppendingTypeTableBuilder &table,
    const TPITarget &target) {

    auto returnIndex = createType(
        func.returnType,
        table,
        target
    );

    if (!returnIndex)
        return returnIndex.takeError();

    std::vector<TypeIndex> argumentIndices;

    argumentIndices.reserve(
        func.argTypes.size() +
        static_cast<size_t>(func.variadic)
    );

    for (const TypeRequest &arg : func.argTypes) {
        auto argIndex = createType(
            arg,
            table,
            target
        );

        if (!argIndex)
            return argIndex.takeError();

        argumentIndices.push_back(*argIndex);
    }

    if (func.variadic) {
        // Завершающий T_NOTYPE обозначает "...".
        argumentIndices.push_back(TypeIndex::None());
    }

    ArgListRecord argList(
        TypeRecordKind::ArgList,
        argumentIndices
    );

    TypeIndex argListIndex = table.writeLeafType(argList);

    ProcedureRecord procedure(
        *returnIndex,
        func.callingConvention,
        FunctionOptions::None,
        static_cast<uint16_t>(argumentIndices.size()),
        argListIndex
    );

    return table.writeLeafType(procedure);
}

} // namespace

Expected<TPITypes> generateTPI(
    pdb::PDBFileBuilder &PdbBuilder,
    BumpPtrAllocator &Allocator,
    const TPITarget &Target,
    const std::vector<SimpleTypeKind> &RequestedSimpleTypes,
    const std::vector<TypeRequest> &RequestedComplexTypes,
    const std::vector<FunctionSignatureRequest> &RequestedFunctions) {

    if (Target.pointerSize != 4 && Target.pointerSize != 8)
        return typeError(
            "Target pointer size must be 4 or 8 bytes"
        );

    // Проверяем запросы до записи в TPI.
    for (const TypeRequest &req : RequestedComplexTypes) {
        if (Error err = validateType(req, Target))
            return std::move(err);
    }

    for (const FunctionSignatureRequest &func
         : RequestedFunctions) {

        if (Error err = validateFunction(func, Target))
            return std::move(err);
    }

    AppendingTypeTableBuilder table(Allocator);
    TPITypes result{};

    result.SimpleTypes.reserve(RequestedSimpleTypes.size());
    result.ComplexTypes.reserve(RequestedComplexTypes.size());
    result.FunctionTypes.reserve(RequestedFunctions.size());

    // Простые типы уже имеют встроенные CodeView TypeIndex.
    for (SimpleTypeKind kind : RequestedSimpleTypes) {
        if (kind == SimpleTypeKind::None)
            return typeError(
                "Requested simple type cannot be None"
            );

        result.SimpleTypes.emplace_back(kind);
    }

    // Сложные типы.
    for (const TypeRequest &req : RequestedComplexTypes) {
        auto index = createType(
            req,
            table,
            Target
        );

        if (!index)
            return index.takeError();

        result.ComplexTypes.push_back(*index);
    }

    // Сигнатуры функций.
    for (const FunctionSignatureRequest &func
         : RequestedFunctions) {

        auto index = createFunctionSignature(
            func,
            table,
            Target
        );

        if (!index)
            return index.takeError();

        result.FunctionTypes.push_back(*index);
    }

    // Перенос всех созданных записей в TPI-поток PDB.
    auto &tpiBuilder = PdbBuilder.getTpiBuilder();

    for (ArrayRef<uint8_t> bytes : table.records()) {
        auto hash = pdb::hashTypeRecord(CVType(bytes));

        if (!hash)
            return hash.takeError();

        tpiBuilder.addTypeRecord(bytes, *hash);
    }

    return result;
}
