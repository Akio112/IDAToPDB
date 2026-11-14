// Публичный интерфейс генератора PDB. Этот файл не включает заголовки LLVM,
// чтобы обёртку (pybind11) можно было собрать без них.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdbgen {

// Один тип. Правила те же, что в контракте Ани (версия 1).
struct Type {
    std::uint32_t id = 0;              // уникальный, не 0
    std::string kind;                  // "Primitive", "Pointer", "LValueReference", "RValueReference", "Array"
    std::string base;                  // имя llvm::codeview::SimpleTypeKind, только для Primitive ("Int32")
    std::uint16_t qualifiers = 0;      // битовая маска: 1 const, 2 volatile, 4 unaligned, 8 restrict
    std::uint32_t elementTypeId = 0;   // id вложенного типа (Pointer, ссылки, Array)
    std::uint64_t sizeBytes = 0;       // полный размер массива в байтах (только Array)
};

// Сигнатура функции. Номер сигнатуры это её позиция в Input::signatures (с нуля).
struct Signature {
    std::uint32_t returnTypeId = 0;
    std::vector<std::uint32_t> argTypeIds;
    std::string callingConvention = "NearC";  // "NearC", "NearPascal", "NearFast", "NearStdCall", "ThisCall"
    bool variadic = false;
};

// Одна функция из IDA.
struct Function {
    std::string name;
    std::uint32_t rva = 0;             // адрес функции минус ImageBase
    std::uint32_t size = 0;            // размер кода в байтах
    std::int32_t signatureIndex = -1;  // номер в Input::signatures, -1 если тип неизвестен
};

struct Input {
    std::string pePath;                // путь к исходному .exe (из него берутся GUID, Age и секции)
    std::string outPath;               // куда записать .pdb
    std::uint8_t pointerSize = 8;      // 4 или 8
    std::vector<Type> types;
    std::vector<Signature> signatures;
    std::vector<Function> functions;
};

// Строит PDB и записывает его в input.outPath.
// Имена модуля и исходного файла в PDB берутся из имени .exe (demo.exe -> demo.obj, demo.c).
// При любой ошибке (неверные данные, нет файла, сбой LLVM) бросает std::runtime_error с понятным текстом.
void generate(const Input& input);

}  // namespace pdbgen
