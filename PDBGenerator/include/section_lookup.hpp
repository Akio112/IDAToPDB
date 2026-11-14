// Перевод RVA (расстояние от начала exe в памяти) в пару "номер секции : смещение внутри секции".
// PDB хранит адреса именно так (см. урок про ASLR): смещение не зависит от того,
// по какому базовому адресу загрузили exe.
#pragma once

#include <cstdint>
#include <optional>

#include "pe_image.hpp"

struct SectionOffset {
    std::uint16_t segment;  // номер секции, нумерация с 1 (.text обычно 1)
    std::uint32_t offset;   // смещение от начала этой секции
};

// Возвращает пару "секция : смещение" или "ничего" (std::nullopt), если RVA не попал ни в одну секцию.
inline std::optional<SectionOffset> rvaToSectionOffset(const PeImage& image, std::uint32_t rva) {
    for (std::size_t i = 0; i < image.sections.size(); ++i) {
        const std::uint32_t start = image.sections[i].VirtualAddress;  // где секция начинается в памяти (RVA)
        const std::uint32_t size = image.sections[i].VirtualSize;      // сколько места занимает в памяти

        if (rva >= start && rva < start + size) {
            return SectionOffset{static_cast<std::uint16_t>(i + 1), rva - start};
        }
    }
    return std::nullopt;
}
