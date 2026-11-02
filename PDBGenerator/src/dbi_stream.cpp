#include "dbi_stream.hpp"

#include <cstddef>
#include <cstdint>

#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptorBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"

using namespace llvm;
using namespace llvm::pdb;

Error buildDbiStream(PDBFileBuilder& builder, const DbiInput& input) {
    const PeImage& image = input.image;
    DbiStreamBuilder& dbi = builder.getDbiBuilder();

    // 1. Заголовок (в нём 64 байта; размеры подпотоков и номера потоков LLVM посчитает сам).
    dbi.setVersionHeader(PdbDbiV70);       // всегда V70 (19990903)
    dbi.setAge(image.age);                  // обязан совпасть с Age в PDB-потоке и в exe
    dbi.setBuildNumber(14, 11);             // как у lld: 0x8E0B
    dbi.setMachineType(image.machine);      // 0x8664 для x64
    dbi.setFlags(0);

    // 2. Модуль = один объектный файл. Для demo.c это demo.obj.
    auto module = dbi.addModuleInfo(input.moduleName);
    if (!module) {
        return module.takeError();
    }
    module->setObjFileName(input.moduleName);

    // 3. Исходные файлы этого модуля (пойдут в подпоток FileInfo).
    for (StringRef source : input.sources) {
        if (Error err = dbi.addModuleSourceFile(*module, source)) {
            return err;
        }
    }

    // 4. SectionContribution: "эта часть секции принадлежит модулю №0".
    //    Упрощение: модуль 0 владеет секцией целиком, от смещения 0 до конца.
    for (std::size_t i = 0; i < image.sections.size(); ++i) {
        const auto& section = image.sections[i];
        SectionContrib contrib{};
        contrib.ISect = static_cast<std::uint16_t>(i + 1);  // секции нумеруются с 1
        contrib.Off = 0;
        contrib.Size = static_cast<std::int32_t>(section.VirtualSize);
        contrib.Characteristics = section.Characteristics;
        contrib.Imod = 0;                                    // номер модуля из пункта 2
        dbi.addSectionContrib(contrib);
    }

    // 5. SectionMap: таблица "логических сегментов" (LLVM строит её из заголовков секций).
    dbi.createSectionMap(image.sections);

    // 6. Копия заголовков секций отдельным потоком. WinDbg по ней переводит
    //    "секция:смещение" из PDB в адрес в памяти, без неё имена функций не находятся.
    const auto* raw = reinterpret_cast<const std::uint8_t*>(image.sections.data());
    const std::size_t bytes = image.sections.size() * sizeof(llvm::object::coff_section);
    return dbi.addDbgStream(DbgHeaderType::SectionHdr, ArrayRef<std::uint8_t>(raw, bytes));
}
