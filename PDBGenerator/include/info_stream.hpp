// ===== Поток 1: PDB Stream (информационный поток) =====
// Метка "этот PDB подходит к этому exe": GUID и Age. Отладчик сравнивает их
// с записью RSDS внутри exe, и если они не совпали, PDB не принимается.
#pragma once

#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Support/Error.h"
#include "pe_image.hpp"

// GUID и Age берутся из PeImage (то есть из exe), поэтому совпадают с DBI и с exe.
llvm::Error buildInfoStream(llvm::pdb::PDBFileBuilder& builder, const PeImage& image);
