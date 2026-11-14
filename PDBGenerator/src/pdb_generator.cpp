// Реализация pdbgen::generate: собирает целый PDB из данных, пришедших в Input.
// Здесь та же последовательность, что раньше была в main.cpp, плюс перевод входных
// структур в формат TPI и превращение ошибок LLVM в исключения.
#include "pdb_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "dbi_stream.hpp"
#include "info_stream.hpp"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include "pe_image.hpp"
#include "section_lookup.hpp"
#include "tpi_stream.hpp"

namespace pdbgen {
namespace {

// ---------- ошибки: из "коробок" LLVM в исключения ----------

[[noreturn]] void throwLlvmError(llvm::Error err) {
    throw std::runtime_error(llvm::toString(std::move(err)));
}

// Если в Error лежит ошибка, бросает исключение. Если всё хорошо, ничего не делает.
void checkError(llvm::Error err) {
    if (err) {
        throwLlvmError(std::move(err));
    }
}

// Достаёт результат из Expected или бросает исключение с текстом ошибки.
template <typename T>
T takeValue(llvm::Expected<T> value) {
    if (!value) {
        throwLlvmError(value.takeError());
    }
    return std::move(*value);
}

// ---------- перевод строк из Input в enum ----------

TypeKind parseKind(const std::string& name) {
    if (name == "Primitive") return TypeKind::Primitive;
    if (name == "Pointer") return TypeKind::Pointer;
    if (name == "LValueReference") return TypeKind::LValueReference;
    if (name == "RValueReference") return TypeKind::RValueReference;
    if (name == "Array") return TypeKind::Array;
    throw std::runtime_error("pdbgen: неизвестный kind типа: \"" + name + "\"");
}

struct NamedBase {
    const char* name;
    llvm::codeview::SimpleTypeKind value;
};

// Имена совпадают с именами llvm::codeview::SimpleTypeKind. Добавлять новые сюда по мере надобности.
#define PDBGEN_BASE(n) {#n, llvm::codeview::SimpleTypeKind::n}
const NamedBase kBases[] = {
    PDBGEN_BASE(Void),
    PDBGEN_BASE(Boolean8),
    PDBGEN_BASE(SignedCharacter),
    PDBGEN_BASE(UnsignedCharacter),
    PDBGEN_BASE(NarrowCharacter),
    PDBGEN_BASE(WideCharacter),
    PDBGEN_BASE(SByte),
    PDBGEN_BASE(Byte),
    PDBGEN_BASE(Int16Short),
    PDBGEN_BASE(UInt16Short),
    PDBGEN_BASE(Int16),
    PDBGEN_BASE(UInt16),
    PDBGEN_BASE(Int32Long),
    PDBGEN_BASE(UInt32Long),
    PDBGEN_BASE(Int32),
    PDBGEN_BASE(UInt32),
    PDBGEN_BASE(Int64Quad),
    PDBGEN_BASE(UInt64Quad),
    PDBGEN_BASE(Int64),
    PDBGEN_BASE(UInt64),
    PDBGEN_BASE(Float32),
    PDBGEN_BASE(Float64),
    PDBGEN_BASE(Float80),
};
#undef PDBGEN_BASE

llvm::codeview::SimpleTypeKind parseBase(const std::string& name) {
    for (const NamedBase& entry : kBases) {
        if (name == entry.name) {
            return entry.value;
        }
    }
    throw std::runtime_error("pdbgen: неизвестное имя примитивного типа (base): \"" + name + "\"");
}

struct NamedConvention {
    const char* name;
    llvm::codeview::CallingConvention value;
};

#define PDBGEN_CONV(n) {#n, llvm::codeview::CallingConvention::n}
const NamedConvention kConventions[] = {
    PDBGEN_CONV(NearC),
    PDBGEN_CONV(NearPascal),
    PDBGEN_CONV(NearFast),
    PDBGEN_CONV(NearStdCall),
    PDBGEN_CONV(ThisCall),
};
#undef PDBGEN_CONV

llvm::codeview::CallingConvention parseConvention(const std::string& name) {
    for (const NamedConvention& entry : kConventions) {
        if (name == entry.name) {
            return entry.value;
        }
    }
    throw std::runtime_error("pdbgen: неизвестное callingConvention: \"" + name + "\"");
}

// ---------- проверка входных данных (то, что не проверяет TPI) ----------

void validateInput(const Input& input) {
    if (input.pePath.empty()) {
        throw std::runtime_error("pdbgen: не задан pePath");
    }
    if (input.outPath.empty()) {
        throw std::runtime_error("pdbgen: не задан outPath");
    }
    if (input.pointerSize != 4 && input.pointerSize != 8) {
        throw std::runtime_error("pdbgen: pointerSize должен быть 4 или 8");
    }
    const std::int32_t signatureCount = static_cast<std::int32_t>(input.signatures.size());
    for (std::size_t i = 0; i < input.functions.size(); ++i) {
        const pdbgen::Function& fn = input.functions[i];
        if (fn.name.empty()) {
            throw std::runtime_error("pdbgen: у функции №" + std::to_string(i) + " пустое имя");
        }
        if (fn.signatureIndex < -1 || fn.signatureIndex >= signatureCount) {
            throw std::runtime_error("pdbgen: у функции \"" + fn.name + "\" signatureIndex = " +
                                     std::to_string(fn.signatureIndex) + " вне диапазона (-1.." +
                                     std::to_string(signatureCount - 1) + ")");
        }
    }
}

}  // namespace

void generate(const Input& input) {
    using namespace llvm;
    using namespace llvm::pdb;

    validateInput(input);

    // Шаг 2. Читаем exe: GUID, Age, таблицу секций.
    PeImage image = takeValue(readPeImage(input.pePath));

    // Шаг 3. Сборщик PDB и служебные потоки 0..4.
    BumpPtrAllocator allocator;
    PDBFileBuilder builder(allocator);
    checkError(builder.initialize(4096));
    for (std::uint32_t i = 0; i < kSpecialStreamCount; ++i) {
        (void)takeValue(builder.getMsfBuilder().addStream(0));
    }
    // Поток 1: GUID и Age.
    checkError(buildInfoStream(builder, image));

    // Шаг 4. Переводим наши структуры в формат.
    std::vector<TypeRequest> typeRequests;
    typeRequests.reserve(input.types.size());
    for (const pdbgen::Type& t : input.types) {
        TypeRequest request;
        request.id = t.id;
        request.kind = parseKind(t.kind);
        if (request.kind == TypeKind::Primitive) {
            request.base = parseBase(t.base);
        }
        request.qualifiers = t.qualifiers;
        request.elementTypeId = t.elementTypeId;
        request.sizeBytes = t.sizeBytes;
        typeRequests.push_back(request);
    }

    std::vector<FunctionSignatureRequest> signatureRequests;
    signatureRequests.reserve(input.signatures.size());
    for (const pdbgen::Signature& s : input.signatures) {
        FunctionSignatureRequest request;
        request.returnTypeId = s.returnTypeId;
        request.argTypeIds = s.argTypeIds;
        request.callingConvention = parseConvention(s.callingConvention);
        request.variadic = s.variadic;
        signatureRequests.push_back(std::move(request));
    }

    // Шаг 5. Поток TPI. .
    TPITarget target;
    target.pointerSize = input.pointerSize;
    TPITypes tpi = takeValue(generateTPI(builder, allocator, target, typeRequests, signatureRequests));
    if (tpi.functionTypes.size() != input.signatures.size()) {
        throw std::runtime_error("pdbgen: TPI вернул неожиданное число сигнатур");
    }
    builder.getIpiBuilder().setVersionHeader(PdbTpiV80);

    // Шаг 6. Из одного списка функций делаем два: публичные символы и символы функций для DBI.
    std::vector<BulkPublic> publics;
    std::vector<FunctionSymbol> symbols;
    publics.reserve(input.functions.size());
    symbols.reserve(input.functions.size());
    for (const pdbgen::Function& fn : input.functions) {
        auto pos = rvaToSectionOffset(image, fn.rva);
        if (!pos) {
            throw std::runtime_error("pdbgen: функция \"" + fn.name + "\" лежит вне секций exe (rva = " +
                                     std::to_string(fn.rva) + ")");
        }
        // Номер типа берём из списка по номеру сигнатуры. -1 значит "типа нет".
        const codeview::TypeIndex type =
            fn.signatureIndex < 0 ? codeview::TypeIndex::None()
                                  : tpi.functionTypes[static_cast<std::size_t>(fn.signatureIndex)];

        BulkPublic pub;
        pub.Name = fn.name.c_str();  // input живёт до конца generate, то есть дольше commit()
        pub.NameLen = static_cast<std::uint32_t>(fn.name.size());
        pub.Segment = pos->segment;
        pub.Offset = pos->offset;
        pub.setFlags(codeview::PublicSymFlags::Function);
        publics.push_back(pub);

        symbols.push_back(FunctionSymbol{StringRef(fn.name), pos->segment, pos->offset, fn.size, type});
    }
    builder.getGsiBuilder().addPublicSymbols(std::move(publics));

    // Шаг 7. Поток DBI. Имена модуля и исходника строим из имени exe: demo.exe -> demo.obj, demo.c.
    const std::string stem = sys::path::stem(input.pePath).str();
    const std::string moduleName = stem + ".obj";
    const std::string sourceName = stem + ".c";
    const StringRef sources[] = {sourceName};
    DbiInput dbiInput{image, moduleName, sources, allocator, symbols};
    checkError(buildDbiStream(builder, dbiInput));

    codeview::GUID written{};
    checkError(builder.commit(input.outPath, &written));
}

}  // namespace pdbgen
