// ===== Поток DBI (поток №3 в PDB) =====
// Это "оглавление" PDB: из каких модулей (объектных файлов) собран exe,
// где в секциях лежит код каждого модуля, какие исходные файлы использовались.
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Error.h"
#include "pe_image.hpp"

struct DbiInput {
    const PeImage& image;
    llvm::StringRef moduleName;                // например "demo.obj"
    llvm::ArrayRef<llvm::StringRef> sources;   // например {"demo.c"}
};

// Заполняет DbiStreamBuilder. Сами байты потока LLVM запишет при builder.commit().
llvm::Error buildDbiStream(llvm::pdb::PDBFileBuilder& builder, const DbiInput& input);
