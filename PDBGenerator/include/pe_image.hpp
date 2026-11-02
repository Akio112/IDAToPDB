// Читаем из готового exe всё, что нужно потоку DBI (и PDB-потоку):
// машину (x64/x86), таблицу секций, GUID и Age из записи CodeView (RSDS).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

struct PeImage {
    llvm::COFF::MachineTypes machine{};
    std::array<std::uint8_t, 16> guid{};
    std::uint32_t age = 0;
    std::vector<llvm::object::coff_section> sections;  // заголовки секций .text, .rdata, ...
};

// Ошибки возвращаем в стиле LLVM: llvm::Expected<T> это либо результат, либо ошибка.
llvm::Expected<PeImage> readPeImage(const std::string& path);
