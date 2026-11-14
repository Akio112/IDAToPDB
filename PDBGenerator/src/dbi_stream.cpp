#include "dbi_stream.hpp"

#include <cstddef>
#include <cstdint>

#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolSerializer.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptorBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"

using namespace llvm;
using namespace llvm::pdb;
using namespace llvm::codeview;

namespace {

// Запись S_END (закрывает функцию) целиком, 4 байта:
//   длина записи без этого поля = 2, тип записи 0x0006 (S_END). Оба числа little-endian.
// Живёт всю программу (static), потому что LLVM хранит ссылку на эти байты до commit().
const std::uint8_t kScopeEndRecord[] = {0x02, 0x00, 0x06, 0x00};

// Кладёт в модуль пару "S_GPROC32 ... S_END" для каждой функции.
// Для каждой функции ещё кладёт в глобальный поток ссылку S_PROCREF: "функция с таким именем
// лежит в модуле №N по такому смещению". WinDbg ищет функцию по имени именно через эти ссылки.
void addFunctionSymbols(DbiModuleDescriptorBuilder& module, GSIStreamBuilder& globals,
                        std::uint16_t moduleNumber, const DbiInput& input) {
    // Поток символов модуля начинается с 4-байтовой сигнатуры (C13).
    // Поле End у функции = смещение её S_END от начала потока, поэтому считаем его с учётом сигнатуры.
    std::uint32_t streamOffset = sizeof(std::uint32_t);

    for (const FunctionSymbol& fn : input.functions) {
        ProcSym proc(SymbolRecordKind::GlobalProcSym);  // S_GPROC32
        proc.Parent = 0;       // функция верхнего уровня, внутри другой функции не лежит
        proc.Next = 0;
        proc.CodeSize = fn.size;
        proc.DbgStart = 0;     // упрощение: начало тела (после пролога) совпадает с началом функции
        proc.DbgEnd = fn.size; // упрощение: конец тела (до эпилога) совпадает с концом функции
        proc.FunctionType = fn.type;
        proc.CodeOffset = fn.offset;
        proc.Segment = fn.segment;
        proc.Flags = ProcSymFlags::None;
        proc.Name = fn.name;

        // Размер записи зависит только от длины имени, поэтому сначала пишем "пробную" копию,
        // узнаём длину, и по ней вычисляем End (S_END идёт сразу за функцией).
        const CVSymbol probe =
            SymbolSerializer::writeOneSymbol(proc, input.allocator, CodeViewContainer::Pdb);
        proc.End = streamOffset + static_cast<std::uint32_t>(probe.length());

        const CVSymbol procRecord =
            SymbolSerializer::writeOneSymbol(proc, input.allocator, CodeViewContainer::Pdb);
        module.addSymbol(procRecord);

        // Ссылка из глобального потока. Смещение считается от начала потока модуля (с сигнатурой),
        // то есть это значение streamOffset до добавления самой записи функции.
        ProcRefSym ref(SymbolRecordKind::ProcRefSym);
        ref.SumName = 0;
        ref.SymOffset = streamOffset;
        ref.Module = moduleNumber;  // номера модулей в S_PROCREF начинаются с 1
        ref.Name = fn.name;
        globals.addGlobalSymbol(
            SymbolSerializer::writeOneSymbol(ref, input.allocator, CodeViewContainer::Pdb));

        streamOffset += static_cast<std::uint32_t>(procRecord.length());

        module.addSymbol(CVSymbol(ArrayRef<std::uint8_t>(kScopeEndRecord)));
        streamOffset += sizeof(kScopeEndRecord);
    }
}

}  // namespace

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

    // 3.5. Символы функций внутри модуля: где функция начинается, какой у неё размер и тип.
    addFunctionSymbols(*module, builder.getGsiBuilder(), 1, input);

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
