#include "info_stream.hpp"

#include <cstdint>
#include <cstring>

#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/PDB/Native/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"

using namespace llvm;
using namespace llvm::pdb;

Error buildInfoStream(PDBFileBuilder& builder, const PeImage& image) {
    codeview::GUID guid{};
    std::memcpy(guid.Guid, image.guid.data(), sizeof guid.Guid);

    auto& info = builder.getInfoBuilder();
    info.setVersion(PdbImplVC70);
    info.addFeature(PdbRaw_FeatureSig::VC140);
    info.setGuid(guid);        // тот же GUID, что в записи RSDS внутри exe
    info.setAge(image.age);    // тот же Age, что в exe и в DBI

    std::uint32_t signature = 0;
    std::memcpy(&signature, image.guid.data(), sizeof signature);
    info.setSignature(signature);
    return Error::success();
}
