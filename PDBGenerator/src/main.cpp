// Собирает целый PDB для demo.exe. Ваша часть (DBI) в dbi_stream.cpp, типы (TPI) в tpi_stream.cpp,
// остальное здесь это "обвязка", чтобы получить файл и сверить его с эталоном.
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "dbi_stream.hpp"
#include "info_stream.hpp"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"
#include "tpi_stream.hpp"

using namespace llvm;
using namespace llvm::pdb;

namespace {

struct PublicFunction {
    std::string_view name;
    std::uint32_t offsetInText;  // смещение внутри .text (секция №1)
};

// В реальном проекте этот список приходит из YAML (IDA). Здесь значения demo.exe.
constexpr std::array<PublicFunction, 4> kDemoFunctions = {{
    {"my_add", 0x00},
    {"my_multiply", 0x20},
    {"my_check_password", 0x40},
    {"main", 0xF0},
}};

int fail(Error err) {
    errs() << "ошибка: " << toString(std::move(err)) << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        errs() << "использование: make_pdb <demo.exe> <выходной.pdb>\n";
        return 2;
    }

    auto image = readPeImage(argv[1]);
    if (!image) {
        return fail(image.takeError());
    }

    BumpPtrAllocator allocator;
    PDBFileBuilder builder(allocator);
    if (Error err = builder.initialize(4096)) {
        return fail(std::move(err));
    }
    // Служебные потоки 0..4 (старый каталог, PDB, TPI, DBI, IPI) должны существовать заранее.
    for (std::uint32_t i = 0; i < kSpecialStreamCount; ++i) {
        if (auto id = builder.getMsfBuilder().addStream(0); !id) {
            return fail(id.takeError());
        }
    }

    // Поток 1 (PDB Stream): GUID и Age.
    if (Error err = buildInfoStream(builder, *image)) {
        return fail(std::move(err));
    }

    // Поток TPI: типы функций (часть Анны). IPI остаётся пустым, но присутствовать обязан.
    auto types = generateTPI(builder, allocator);
    if (!types) {
        return fail(types.takeError());
    }
    builder.getIpiBuilder().setVersionHeader(PdbTpiV80);

    // Публичные символы (имя + секция:смещение). Нужны WinDbg, чтобы показать имена.
    std::vector<BulkPublic> publics;
    for (const auto& fn : kDemoFunctions) {
        BulkPublic pub;
        pub.Name = fn.name.data();
        pub.NameLen = static_cast<std::uint32_t>(fn.name.size());
        pub.Segment = 1;
        pub.Offset = fn.offsetInText;
        pub.setFlags(codeview::PublicSymFlags::Function);
        publics.push_back(pub);
    }
    builder.getGsiBuilder().addPublicSymbols(std::move(publics));

    // ===== Поток DBI: ваша часть =====
    const StringRef sources[] = {"demo.c"};
    DbiInput input{*image, "demo.obj", sources};
    if (Error err = buildDbiStream(builder, input)) {
        return fail(std::move(err));
    }

    codeview::GUID written{};
    if (Error err = builder.commit(argv[2], &written)) {
        return fail(std::move(err));
    }
    outs() << "готово: " << argv[2] << " (age = " << image->age << ")\n";
    return 0;
}
