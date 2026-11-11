
#include "tpi_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"

using namespace llvm;
using namespace llvm::codeview;

namespace {

constexpr unsigned MaxTypeDepth = 64;

constexpr TypeQualifiers KnownQualifiers =
    static_cast<TypeQualifiers>(TypeQualifier::Const) |
    static_cast<TypeQualifiers>(TypeQualifier::Volatile) |
    static_cast<TypeQualifiers>(TypeQualifier::Unaligned) |
    static_cast<TypeQualifiers>(TypeQualifier::Restrict);

using TypeLookup =
    std::unordered_map<TypeId, const TypeRequest *>;

// Создание ошибки с понятным текстом.
Error typeError(const std::string &message) {
    return createStringError(
        std::errc::invalid_argument,
        "%s",
        message.c_str()
    );
}

bool isReference(TypeKind kind) {
    return kind == TypeKind::LValueReference ||
           kind == TypeKind::RValueReference;
}

bool isPointerLike(TypeKind kind) {
    return kind == TypeKind::Pointer ||
           isReference(kind);
}

bool isVoid(const TypeRequest &type) {
    return type.kind == TypeKind::Primitive &&
           type.base == SimpleTypeKind::Void;
}

// Модификаторы обычных типов: const, volatile, unaligned.
ModifierOptions modifierOptions(TypeQualifiers qualifiers) {
    ModifierOptions options = ModifierOptions::None;

    if (hasQualifier(qualifiers, TypeQualifier::Const))
        options |= ModifierOptions::Const;

    if (hasQualifier(qualifiers, TypeQualifier::Volatile))
        options |= ModifierOptions::Volatile;

    if (hasQualifier(qualifiers, TypeQualifier::Unaligned))
        options |= ModifierOptions::Unaligned;

    return options;
}

// Модификаторы указателей.
PointerOptions pointerOptions(TypeQualifiers qualifiers) {
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

// Проверяет поля одного TypeRequest.
//
// Здесь мы ещё не проверяем, существует ли elementTypeId.
// Этим займётся TypeResolver.
Error validateTypeRequest(const TypeRequest &type) {
    if ((type.qualifiers & ~KnownQualifiers) != 0) {
        return typeError(
            "Unknown qualifiers for type ID " +
            std::to_string(type.id)
        );
    }

    switch (type.kind) {
    case TypeKind::Primitive:
        if (type.base == SimpleTypeKind::None ||
            type.elementTypeId != 0 ||
            type.sizeBytes != 0) {
            return typeError(
                "Invalid primitive type ID " +
                std::to_string(type.id)
            );
        }

        if (hasQualifier(
                type.qualifiers,
                TypeQualifier::Restrict)) {
            return typeError(
                "Restrict is only supported on pointers"
            );
        }

        return Error::success();

    case TypeKind::Pointer:
    case TypeKind::LValueReference:
    case TypeKind::RValueReference:
        if (type.base != SimpleTypeKind::None ||
            type.elementTypeId == 0 ||
            type.sizeBytes != 0) {
            return typeError(
                "Invalid pointer/reference type ID " +
                std::to_string(type.id)
            );
        }

        if (isReference(type.kind) &&
            type.qualifiers != 0) {
            return typeError(
                "Reference cannot have top-level qualifiers"
            );
        }

        return Error::success();

    case TypeKind::Array:
        if (type.base != SimpleTypeKind::None ||
            type.elementTypeId == 0 ||
            type.qualifiers != 0 ||
            type.sizeBytes == 0) {
            return typeError(
                "Invalid array type ID " +
                std::to_string(type.id)
            );
        }

        return Error::success();
    }

    return typeError(
        "Unsupported TypeKind for type ID " +
        std::to_string(type.id)
    );
}

// Проверяет, можно ли вложить element в parent.
//
// Например:
// int*  — можно
// int&* — нельзя
// void& — нельзя
// void[5] — нельзя
Error validateElementRelationship(
    const TypeRequest &parent,
    const TypeRequest &element) {

    if (isReference(element.kind)) {
        return typeError(
            "Pointer/reference/array of reference is invalid"
        );
    }

    if ((parent.kind == TypeKind::Array ||
         isReference(parent.kind)) &&
        isVoid(element)) {
        return typeError(
            "Array/reference of void is invalid"
        );
    }

    return Error::success();
}

// Строит таблицу TypeId -> TypeRequest.
//
// Дополнительно проверяет:
// - ID не равен 0;
// - нет повторяющихся ID;
// - поля каждого типа корректны.
Expected<TypeLookup> buildTypeLookup(
    const std::vector<TypeRequest> &types) {

    TypeLookup lookup;
    lookup.reserve(types.size());

    for (const TypeRequest &type : types) {
        if (type.id == 0) {
            return typeError("Type ID 0 is reserved");
        }

        if (!lookup.emplace(type.id, &type).second) {
            return typeError(
                "Duplicate type ID " +
                std::to_string(type.id)
            );
        }

        if (Error err = validateTypeRequest(type)) {
            return std::move(err);
        }
    }

    return lookup;
}

// Разрешает зависимости между типами и создаёт
// соответствующие CodeView TypeIndex.
class TypeResolver {
public:
    TypeResolver(
        const TPITarget &target,
        const TypeLookup &types,
        AppendingTypeTableBuilder &table,
        TPITypes &result)
        : target_(target),
          types_(types),
          table_(table),
          result_(result) {}

    // Основной метод:
    // TypeId -> CodeView TypeIndex.
    Expected<TypeIndex> resolve(
        TypeId id,
        unsigned depth = 0) {

        // Если тип уже создан, возвращаем его индекс.
        auto cached = result_.typeIndices.find(id);

        if (cached != result_.typeIndices.end()) {
            return cached->second;
        }

        if (depth >= MaxTypeDepth) {
            return typeError("Type nesting is too deep");
        }

        // Находим входное описание типа.
        auto it = types_.find(id);

        if (it == types_.end()) {
            return typeError(
                "Unknown type ID " +
                std::to_string(id)
            );
        }

        // Если этот тип уже находится в процессе
        // обработки, значит обнаружена циклическая
        // зависимость.
        if (states_[id] == State::Visiting) {
            return typeError(
                "Cyclic type dependency at ID " +
                std::to_string(id)
            );
        }

        states_[id] = State::Visiting;

        // Создаём CodeView-запись нужного вида.
        auto index = createType(*it->second, depth);

        if (!index) {
            states_.erase(id);
            return index.takeError();
        }

        // Сохраняем результат, чтобы не создавать
        // один и тот же тип повторно.
        result_.typeIndices.emplace(id, *index);

        states_[id] = State::Done;

        return *index;
    }

private:
    enum class State : uint8_t {
        Visiting,
        Done
    };

    // Определяет, какой именно тип нужно создать.
    Expected<TypeIndex> createType(
        const TypeRequest &type,
        unsigned depth) {

        if (type.kind == TypeKind::Primitive) {
            return createPrimitive(type);
        }

        // У сложного типа должен существовать
        // вложенный тип.
        auto child = types_.find(type.elementTypeId);

        if (child == types_.end()) {
            return typeError(
                "Unknown elementTypeId " +
                std::to_string(type.elementTypeId)
            );
        }

        if (Error err = validateElementRelationship(
                type,
                *child->second)) {
            return std::move(err);
        }

        // Сначала создаём вложенный тип.
        auto elementIndex = resolve(
            type.elementTypeId,
            depth + 1
        );

        if (!elementIndex) {
            return elementIndex.takeError();
        }

        // Затем создаём внешний тип.
        if (isPointerLike(type.kind)) {
            return createPointerOrReference(
                type,
                *elementIndex
            );
        }

        if (type.kind == TypeKind::Array) {
            return createArray(
                type,
                *elementIndex
            );
        }

        return typeError("Unsupported TypeKind");
    }

    // Примитивный тип или LF_MODIFIER.
    TypeIndex createPrimitive(const TypeRequest &type) {
        TypeIndex index(type.base);

        ModifierOptions options =
            modifierOptions(type.qualifiers);

        if (options != ModifierOptions::None) {
            ModifierRecord modifier(index, options);
            index = table_.writeLeafType(modifier);
        }

        return index;
    }

    // LF_POINTER для указателей и ссылок.
    TypeIndex createPointerOrReference(
        const TypeRequest &type,
        TypeIndex elementIndex) {

        const PointerKind kind =
            target_.pointerSize == 8
                ? PointerKind::Near64
                : PointerKind::Near32;

        PointerMode mode = PointerMode::Pointer;

        if (type.kind == TypeKind::LValueReference) {
            mode = PointerMode::LValueReference;
        }
        else if (type.kind == TypeKind::RValueReference) {
            mode = PointerMode::RValueReference;
        }

        PointerRecord pointer(
            elementIndex,
            kind,
            mode,
            pointerOptions(type.qualifiers),
            target_.pointerSize
        );

        return table_.writeLeafType(pointer);
    }

    // LF_ARRAY.
    TypeIndex createArray(
        const TypeRequest &type,
        TypeIndex elementIndex) {

        // LF_ARRAY хранит полный размер массива
        // в байтах, а не число элементов.
        ArrayRecord array(
            elementIndex,
            TypeIndex(SimpleTypeKind::UInt32),
            type.sizeBytes,
            ""
        );

        return table_.writeLeafType(array);
    }

    const TPITarget &target_;
    const TypeLookup &types_;
    AppendingTypeTableBuilder &table_;
    TPITypes &result_;

    // Отслеживает типы, находящиеся в обработке.
    std::unordered_map<TypeId, State> states_;
};

// Проверяет корректность сигнатуры функции.
Error validateFunction(
    const FunctionSignatureRequest &func,
    const TypeLookup &types) {

    auto ret = types.find(func.returnTypeId);

    if (ret == types.end()) {
        return typeError(
            "Unknown function return type ID " +
            std::to_string(func.returnTypeId)
        );
    }

    if (ret->second->kind == TypeKind::Array) {
        return typeError(
            "Function cannot return an array by value"
        );
    }

    const size_t maxArgs =
        std::numeric_limits<uint16_t>::max();

    if (func.argTypeIds.size() > maxArgs ||
        (func.variadic &&
         func.argTypeIds.size() == maxArgs)) {
        return typeError(
            "Too many function arguments"
        );
    }

    for (TypeId id : func.argTypeIds) {
        auto arg = types.find(id);

        if (arg == types.end()) {
            return typeError(
                "Unknown function argument type ID " +
                std::to_string(id)
            );
        }

        if (isVoid(*arg->second)) {
            return typeError(
                "Void cannot be an explicit argument"
            );
        }
    }

    return Error::success();
}

// Преобразует ID аргументов функции
// в настоящие CodeView TypeIndex.
Expected<std::vector<TypeIndex>> resolveArguments(
    const FunctionSignatureRequest &func,
    TypeResolver &resolver) {

    std::vector<TypeIndex> indices;

    indices.reserve(
        func.argTypeIds.size() +
        static_cast<size_t>(func.variadic)
    );

    for (TypeId id : func.argTypeIds) {
        auto index = resolver.resolve(id);

        if (!index) {
            return index.takeError();
        }

        indices.push_back(*index);
    }

    // Завершающий TypeIndex::None() обозначает "...".
    if (func.variadic) {
        indices.push_back(TypeIndex::None());
    }

    return indices;
}

// Создаёт сигнатуру функции:
// LF_ARGLIST + LF_PROCEDURE.
Expected<TypeIndex> createFunctionSignature(
    const FunctionSignatureRequest &func,
    TypeResolver &resolver,
    AppendingTypeTableBuilder &table) {

    // Тип возвращаемого значения.
    auto returnIndex = resolver.resolve(
        func.returnTypeId
    );

    if (!returnIndex) {
        return returnIndex.takeError();
    }

    // Типы аргументов.
    auto args = resolveArguments(func, resolver);

    if (!args) {
        return args.takeError();
    }

    // LF_ARGLIST содержит список типов аргументов.
    ArgListRecord argList(
        TypeRecordKind::ArgList,
        *args
    );

    TypeIndex argListIndex =
        table.writeLeafType(argList);

    // LF_PROCEDURE описывает сигнатуру функции
    // и ссылается на LF_ARGLIST.
    ProcedureRecord procedure(
        *returnIndex,
        func.callingConvention,
        FunctionOptions::None,
        static_cast<uint16_t>(args->size()),
        argListIndex
    );

    return table.writeLeafType(procedure);
}

// Переносит записи из временной таблицы
// в TPI-поток создаваемого PDB.
Error writeTpiRecords(
    pdb::PDBFileBuilder &pdbBuilder,
    AppendingTypeTableBuilder &table) {

    auto &tpiBuilder =
        pdbBuilder.getTpiBuilder();

    for (ArrayRef<uint8_t> bytes : table.records()) {
        auto hash = pdb::hashTypeRecord(
            CVType(bytes)
        );

        if (!hash) {
            return hash.takeError();
        }

        tpiBuilder.addTypeRecord(bytes, *hash);
    }

    return Error::success();
}

} // namespace

// Главная функция генерации TPI.
Expected<TPITypes> generateTPI(
    pdb::PDBFileBuilder &PdbBuilder,
    BumpPtrAllocator &Allocator,
    const TPITarget &Target,
    const std::vector<TypeRequest> &RequestedTypes,
    const std::vector<FunctionSignatureRequest>
        &RequestedFunctions) {

    if (Target.pointerSize != 4 &&
        Target.pointerSize != 8) {
        return typeError(
            "Target pointer size must be 4 or 8 bytes"
        );
    }

    // Строим таблицу ID -> TypeRequest.
    // Проверяем уникальность ID и корректность полей.
    auto types = buildTypeLookup(RequestedTypes);

    if (!types) {
        return types.takeError();
    }

    // Проверяем сигнатуры до записи CodeView-записей.
    for (const FunctionSignatureRequest &func
         : RequestedFunctions) {

        if (Error err = validateFunction(func, *types)) {
            return std::move(err);
        }
    }

    AppendingTypeTableBuilder table(Allocator);

    TPITypes result{};

    result.typeIndices.reserve(
        RequestedTypes.size()
    );

    result.functionTypes.reserve(
        RequestedFunctions.size()
    );

    TypeResolver resolver(
        Target,
        *types,
        table,
        result
    );

    // Создаём типы. Порядок входного списка не важен.
    for (const TypeRequest &type : RequestedTypes) {
        auto index = resolver.resolve(type.id);

        if (!index) {
            return index.takeError();
        }
    }

    // Создаём сигнатуры функций.
    for (const FunctionSignatureRequest &func
         : RequestedFunctions) {

        auto index = createFunctionSignature(
            func,
            resolver,
            table
        );

        if (!index) {
            return index.takeError();
        }

        // Сохраняем TypeIndex в порядке входных функций.
        result.functionTypes.push_back(*index);
    }

    // Записываем готовые CodeView-записи в TPI.
    if (Error err = writeTpiRecords(PdbBuilder, table)) {
        return std::move(err);
    }

    return result;
}
