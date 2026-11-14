// ===== Поток DBI (поток №3 в PDB) =====
// Это "оглавление" PDB: из каких модулей (объектных файлов) собран exe,
// где в секциях лежит код каждого модуля, какие исходные файлы использовались.
#pragma once

#include <cstdint>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "pe_image.hpp"

// Одна функция для символьного потока модуля (запись S_GPROC32).
// Позже эти данные будут приходить из экспорта IDA.
struct FunctionSymbol {
    llvm::StringRef name;
    std::uint16_t segment;           // номер секции, с 1 (.text = 1)
    std::uint32_t offset;            // смещение начала функции внутри секции
    std::uint32_t size;              // размер кода функции в байтах
    llvm::codeview::TypeIndex type;  // номер сигнатуры функции в TPI (часть Анны)
};

struct DbiInput {
    const PeImage& image;
    llvm::StringRef moduleName;                // например "demo.obj"
    llvm::ArrayRef<llvm::StringRef> sources;   // например {"demo.c"}
    llvm::BumpPtrAllocator& allocator;         // хранит байты символов, должен жить до builder.commit()
    llvm::ArrayRef<FunctionSymbol> functions;  // функции модуля
};

// Заполняет DbiStreamBuilder. Сами байты потока LLVM запишет при builder.commit().
llvm::Error buildDbiStream(llvm::pdb::PDBFileBuilder& builder, const DbiInput& input);
