# PDBGenerator

Строит PDB-файл для готового `.exe` по данным из IDA (имена, адреса, размеры и типы функций),
чтобы WinDbg показывал функции по имени, с размером и типом. Только Windows, C++17, LLVM 18.1.8.

## Что делает

Одна функция `pdbgen::generate(const Input&)` (объявлена в `include/pdb_generator.hpp`) принимает данные
и записывает `.pdb`. При ошибке бросает `std::runtime_error` с текстом. Состав `Input`, формат типов и допустимые
значения полей описаны комментариями в заголовке.

```
Input  ->  generate  ->  .pdb
              |
              +-- чтение .exe (GUID, Age, секции)       pe_image, info_stream
              +-- типы и сигнатуры функций (TPI)        tpi_stream
              +-- функции, модуль, секции (DBI)          dbi_stream, section_lookup
```

| Файл | За что отвечает |
|---|---|
| `include/pdb_generator.hpp`, `src/pdb_generator.cpp` | функция `generate` и её входные структуры |
| `src/tpi_stream.cpp` | типы и сигнатуры функций (поток TPI) |
| `src/dbi_stream.cpp` | модуль, секции, записи функций `S_GPROC32` и ссылки `S_PROCREF` (поток DBI) |
| `include/section_lookup.hpp` | перевод RVA в пару «секция : смещение» |
| `src/pe_image.cpp`, `src/info_stream.cpp` | чтение `.exe`, поток с GUID и Age |
| `src/main.cpp` | тестовый запуск на данных `demo.exe` |

## Сборка

Нужны CMake, Ninja, clang и собранный LLVM 18.1.8. Путь к LLVM передаётся через `-DLLVM_DIR`:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DLLVM_DIR=C:/llvm-18.1.8/lib/cmake/llvm
cmake --build build
```

Получаются статическая библиотека `pdbgen` и тестовый exe `make_pdb`.

## Тестовый запуск

```powershell
build\make_pdb.exe demo.exe result.pdb
```

Адреса и размеры функций в `src/main.cpp` зашиты для нашего `demo.exe`, для другого файла они будут неверными.
В аргументах командной строки лучше не использовать русские буквы в путях: Windows передаёт их в другой кодировке.

Проверка в WinDbg:

```
.reload /f demo.exe
x /v demo!my_*
dt demo!my_add
```

## Что не проверено

- Функции без типа (`signatureIndex = -1`): записывается `TypeIndex::None()`, поведение WinDbg не проверено.
- Ошибочные входные данные: проверки есть в коде, тестов нет.
- Работа с `.exe`, кроме `demo.exe`.
- Сохранение `const` для `const char*` не подтверждено (WinDbg его не показывает).

