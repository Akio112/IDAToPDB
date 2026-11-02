
#include "tpi_stream.hpp"

#include <vector>
#include <map>

#include "llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"

using namespace llvm;
using namespace llvm::codeview;

static TypeIndex createType(
    const TypeRequest &req,
    AppendingTypeTableBuilder &Table) {

    TypeIndex current(req.base);

    if (req.isConst) {
        ModifierRecord modRec(current, ModifierOptions::Const);
        current = Table.writeLeafType(modRec);
    }
    if (req.isPointer) {
        PointerRecord ptrRec(
            current, PointerKind::Near64, PointerMode::Pointer,
            PointerOptions::None, 8);
        current = Table.writeLeafType(ptrRec);
    }

    return current;
}

static Expected<TypeIndex> getOrCreateType(
    const TypeRequest &req,
    AppendingTypeTableBuilder &Table,
    std::map<SimpleTypeKind, TypeIndex> &simpleTypes,
    std::map<TypeRequest, TypeIndex> &complexTypes) {

    // Базовый тип — не требует создания записи.
    if (!req.isPointer && !req.isConst) {
        auto it = simpleTypes.find(req.base);
        if (it != simpleTypes.end()) return it->second;
        TypeIndex ti(req.base);
        simpleTypes[req.base] = ti;
        return ti;
    }

    // Сложный тип — проверяем кэш, если нет — создаём через createType.
    auto it = complexTypes.find(req);
    if (it != complexTypes.end()) return it->second;

    TypeIndex ti = createType(req, Table);
    complexTypes[req] = ti;
    return ti;
}
static TypeIndex createFunctionSignature(
    TypeIndex returnType,
    const std::vector<TypeIndex> &argTypes,
    AppendingTypeTableBuilder &Table) {

    ArgListRecord argListRec(TypeRecordKind::ArgList, argTypes);
    TypeIndex argListType = Table.writeLeafType(argListRec);

    ProcedureRecord procRec(
        returnType,
        CallingConvention::NearC,
        FunctionOptions::None,
        static_cast<uint16_t>(argTypes.size()),
        argListType);
    return Table.writeLeafType(procRec);
}

static Expected<TypeIndex> getOrCreateFunctionSignature(
    const FunctionSignatureRequest &func,
    AppendingTypeTableBuilder &Table,
    std::map<SimpleTypeKind, TypeIndex> &simpleTypes,
    std::map<TypeRequest, TypeIndex> &complexTypes,
    std::map<FunctionSignatureRequest, TypeIndex> &functionTypes) {

    auto it = functionTypes.find(func);
    if (it != functionTypes.end()) return it->second;

    auto retTi = getOrCreateType(func.returnType, Table, simpleTypes, complexTypes);
    if (!retTi) return retTi.takeError();

    std::vector<TypeIndex> argTis;
    for (const TypeRequest &arg : func.argTypes) {
        auto argTi = getOrCreateType(arg, Table, simpleTypes, complexTypes);
        if (!argTi) return argTi.takeError();
        argTis.push_back(*argTi);
    }

    TypeIndex funcSigType = createFunctionSignature(*retTi, argTis, Table);
    functionTypes[func] = funcSigType;
    return funcSigType;
}
llvm::Expected<TPITypes>  generateTPI(
    llvm::pdb::PDBFileBuilder &PdbBuilder,
    llvm::BumpPtrAllocator &Allocator,
    const std::vector<llvm::codeview::SimpleTypeKind> &RequestedSimpleTypes,
    const std::vector<TypeRequest> &RequestedComplexTypes,
    const std::vector<FunctionSignatureRequest> &RequestedFunctions){
    llvm::codeview::AppendingTypeTableBuilder Table(Allocator);
    TPITypes result{};
    // Базовые типы 
    for (llvm::codeview::SimpleTypeKind kind : RequestedSimpleTypes) {
        result.SimpleTypes[kind] = llvm::codeview::TypeIndex(kind);
    }
    //Сложные типы
   for (const TypeRequest &req : RequestedComplexTypes) {
        auto ti = getOrCreateType(req, Table, result.SimpleTypes, result.ComplexTypes);
        if (!ti) return ti.takeError();
    }
    //Сигнатуры функций
   for (const FunctionSignatureRequest &func : RequestedFunctions) {
        auto ti = getOrCreateFunctionSignature(
            func, Table, result.SimpleTypes, result.ComplexTypes, result.FunctionTypes);
        if (!ti) return ti.takeError();
    }


    // Перенос в TPI-поток PDB
    auto &TpiTypes = PdbBuilder.getTpiBuilder();
    for (ArrayRef<uint8_t> Bytes : Table.records()) {
        auto Hash = llvm::pdb::hashTypeRecord(CVType(Bytes));
        if (!Hash) return Hash.takeError();
        TpiTypes.addTypeRecord(Bytes, *Hash);
    }

    return result;
}
