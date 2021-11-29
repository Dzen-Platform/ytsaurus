#include "chunk_merger_traversal_info.h"

#include <yt/yt/core/misc/serialize.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

void TChunkMergerTraversalInfo::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, ChunkCount);
    Save(context, ConfigVersion);
}

void TChunkMergerTraversalInfo::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, ChunkCount);
    Load(context, ConfigVersion);
}

void FormatValue(TStringBuilderBase* builder, const TChunkMergerTraversalInfo& traversalInfo, TStringBuf /*spec*/)
{
    builder->AppendFormat("{ChunkCount: %v, ConfigVersion: %v}",
        traversalInfo.ChunkCount,
        traversalInfo.ConfigVersion);
}

TString ToString(const TChunkMergerTraversalInfo& traversalInfo)
{
    return ToStringViaBuilder(traversalInfo);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
