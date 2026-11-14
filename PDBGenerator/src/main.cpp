// Тестовый запуск генератора: заполняет Input данными demo.exe (так же, как потом это будет делать
// скрипт Амра из IDA) и вызывает pdbgen::generate. Результат проверяется в WinDbg.
//
// Адреса и размеры ниже верны для нашего demo.exe (см. reference/demo.pdb), для другого exe они будут другими.
#include <cstdint>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

#include "pdb_generator.hpp"

namespace {

pdbgen::Type makeType(std::uint32_t id, const char* kind, const char* base, std::uint16_t qualifiers,
                      std::uint32_t elementTypeId) {
    pdbgen::Type type;
    type.id = id;
    type.kind = kind;
    type.base = base;
    type.qualifiers = qualifiers;
    type.elementTypeId = elementTypeId;
    return type;
}

pdbgen::Signature makeSignature(std::uint32_t returnTypeId, std::vector<std::uint32_t> args) {
    pdbgen::Signature signature;
    signature.returnTypeId = returnTypeId;
    signature.argTypeIds = std::move(args);
    return signature;
}

pdbgen::Function makeFunction(const char* name, std::uint32_t rva, std::uint32_t size,
                              std::int32_t signatureIndex) {
    pdbgen::Function fn;
    fn.name = name;
    fn.rva = rva;
    fn.size = size;
    fn.signatureIndex = signatureIndex;
    return fn;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "использование: make_pdb <demo.exe> <выходной.pdb>\n";
        return 2;
    }

    pdbgen::Input input;
    input.pePath = argv[1];
    input.outPath = argv[2];
    input.pointerSize = 8;

    // Типы: 1 = int, 2 = const char, 3 = const char*.
    input.types.push_back(makeType(1, "Primitive", "Int32", 0, 0));
    input.types.push_back(makeType(2, "Primitive", "NarrowCharacter", 1, 0));
    input.types.push_back(makeType(3, "Pointer", "", 0, 2));

    // Сигнатуры: 0 = int(int, int), 1 = int(const char*), 2 = int().
    input.signatures.push_back(makeSignature(1, {1, 1}));
    input.signatures.push_back(makeSignature(1, {3}));
    input.signatures.push_back(makeSignature(1, {}));

    // Функции (rva и размеры из эталонного PDB).
    input.functions.push_back(makeFunction("my_add", 4096, 17, 0));
    input.functions.push_back(makeFunction("my_multiply", 4128, 18, 0));
    input.functions.push_back(makeFunction("my_check_password", 4160, 174, 1));
    input.functions.push_back(makeFunction("main", 4336, 103, 2));

    try {
        pdbgen::generate(input);
    } catch (const std::exception& error) {
        std::cerr << "ошибка: " << error.what() << '\n';
        return 1;
    }
    std::cout << "готово: " << input.outPath << '\n';
    return 0;
}
