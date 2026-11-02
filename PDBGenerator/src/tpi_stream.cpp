// Код Анны (generateTPI) с исправлениями под LLVM 18 / C++17.
#include "tpi_stream.hpp"

#include <vector>

#include "llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"

llvm::Expected<FunctionTypes> generateTPI(llvm::pdb::PDBFileBuilder &PdbBuilder,
                                          llvm::BumpPtrAllocator &Allocator) {
    // Записи сначала превращаем в байты в отдельной таблице, номера выдаются с 0x1000.
    // Поток TPI (TpiStreamBuilder) принимает только готовые байты, поэтому в конце переносим их туда.
    llvm::codeview::AppendingTypeTableBuilder Table(Allocator);

    llvm::codeview::TypeIndex IntType(llvm::codeview::SimpleTypeKind::Int32);
    llvm::codeview::TypeIndex CharType(llvm::codeview::SimpleTypeKind::NarrowCharacter);

    // Создаем указатель на const char (const char*) для функции my_check_password(const char *s)
    // Шаг 1: Описываем модификатор const для char
    llvm::codeview::ModifierRecord ModRec(CharType, llvm::codeview::ModifierOptions::Const);
    llvm::codeview::TypeIndex ConstCharType = Table.writeLeafType(ModRec);

    // Шаг 2: указатель на const char (64-битный, размер 8 байт)
    llvm::codeview::PointerRecord PointerRec(
        ConstCharType,                          // Ссылка на const char
        llvm::codeview::PointerKind::Near64,
        llvm::codeview::PointerMode::Pointer,
        llvm::codeview::PointerOptions::None,
        8);
    llvm::codeview::TypeIndex ConstCharPtrType = Table.writeLeafType(PointerRec);

    // Сигнатура int(const char*) для my_check_password
    std::vector<llvm::codeview::TypeIndex> CheckPwdArgs = {ConstCharPtrType};
    llvm::codeview::ArgListRecord CheckPwdArgListRec(llvm::codeview::TypeRecordKind::ArgList,
                                                      CheckPwdArgs);
    llvm::codeview::TypeIndex CheckPwdArgListType = Table.writeLeafType(CheckPwdArgListRec);
    llvm::codeview::ProcedureRecord CheckPwdProcRec(
        IntType,
        llvm::codeview::CallingConvention::NearC,
        llvm::codeview::FunctionOptions::None,
        1,
        CheckPwdArgListType);
    llvm::codeview::TypeIndex CheckPwdFuncSigType = Table.writeLeafType(CheckPwdProcRec);

    // Список аргументов (LF_ARGLIST) для функции int(int, int) -> принимает два int
    std::vector<llvm::codeview::TypeIndex> Args = {IntType, IntType};
    llvm::codeview::ArgListRecord ArgListRec(llvm::codeview::TypeRecordKind::ArgList, Args);
    llvm::codeview::TypeIndex ArgListType = Table.writeLeafType(ArgListRec);
    // Сигнатура функции (LF_PROCEDURE): возвращает int, принимает ArgListType
    llvm::codeview::ProcedureRecord ProcRec(
        IntType,                                    // Возвращаемый тип (int)
        llvm::codeview::CallingConvention::NearC,   // Соглашение о вызовах __cdecl
        llvm::codeview::FunctionOptions::None,
        2,                                          // Количество параметров (2)
        ArgListType);                               // Ссылка на список аргументов
    llvm::codeview::TypeIndex FuncSigType = Table.writeLeafType(ProcRec);

    // Сигнатура int() для main
    std::vector<llvm::codeview::TypeIndex> NoArgs;
    llvm::codeview::ArgListRecord MainArgListRec(llvm::codeview::TypeRecordKind::ArgList, NoArgs);
    llvm::codeview::TypeIndex MainArgListType = Table.writeLeafType(MainArgListRec);
    llvm::codeview::ProcedureRecord MainProcRec(
        IntType,                                    // Возвращает int
        llvm::codeview::CallingConvention::NearC,   // __cdecl
        llvm::codeview::FunctionOptions::None,
        0,                                          // Количество параметров: 0
        MainArgListType);                           // Ссылка на пустой список аргументов
    llvm::codeview::TypeIndex MainFuncSigType = Table.writeLeafType(MainProcRec);

    // Переносим готовые записи в поток TPI самого PDB, у каждой свой хэш.
    auto &TpiTypes = PdbBuilder.getTpiBuilder();
    for (llvm::ArrayRef<uint8_t> Bytes : Table.records()) {
        auto Hash = llvm::pdb::hashTypeRecord(llvm::codeview::CVType(Bytes));
        if (!Hash) {
            return Hash.takeError();
        }
        TpiTypes.addTypeRecord(Bytes, *Hash);
    }
    return FunctionTypes{FuncSigType, CheckPwdFuncSigType, MainFuncSigType};
}
