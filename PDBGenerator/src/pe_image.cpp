#include "pe_image.hpp"

#include <algorithm>

#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;

Expected<PeImage> readPeImage(const std::string& path) {
    auto file = MemoryBuffer::getFile(path);
    if (!file) {
        return createStringError(file.getError(), "не могу открыть %s", path.c_str());
    }

    auto coff = object::COFFObjectFile::create((*file)->getMemBufferRef());
    if (!coff) {
        return createStringError(inconvertibleErrorCode(), "это не PE-файл: %s",
                                 toString(coff.takeError()).c_str());
    }

    PeImage image;
    image.machine = static_cast<COFF::MachineTypes>((*coff)->getMachine());

    for (const auto& section : (*coff)->sections()) {
        image.sections.push_back(*(*coff)->getCOFFSection(section));
    }

    // Запись RSDS: именно по ней отладчик решает, подходит ли PDB к exe.
    const codeview::DebugInfo* info = nullptr;
    StringRef pdbName;
    if (Error err = (*coff)->getDebugPDBInfo(info, pdbName)) {
        return createStringError(inconvertibleErrorCode(), "в exe нет записи CodeView/RSDS: %s",
                                 toString(std::move(err)).c_str());
    }
    if (info == nullptr) {
        return createStringError(inconvertibleErrorCode(),
                                 "в exe нет записи CodeView/RSDS (слинкуйте с /debug)");
    }
    std::copy_n(info->PDB70.Signature, image.guid.size(), image.guid.begin());
    image.age = info->PDB70.Age;
    return image;
}
