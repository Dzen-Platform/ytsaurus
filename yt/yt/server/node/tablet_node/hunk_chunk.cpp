#include "hunk_chunk.h"

#include "serialize.h"

#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>

namespace NYT::NTabletNode {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

THunkChunk::THunkChunk(
    TChunkId id,
    const NTabletNode::NProto::TAddHunkChunkDescriptor* descriptor)
    : Id_(id)
{
    if (descriptor) {
        ChunkMeta_ = descriptor->chunk_meta();
    }
}

void THunkChunk::Initialize()
{
    auto miscExt = GetProtoExtension<NTableClient::NProto::THunkChunkMiscExt>(ChunkMeta_.extensions());
    HunkCount_ = miscExt.hunk_count();
    TotalHunkLength_ = miscExt.total_hunk_length();
}

void THunkChunk::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, State_);
    Save(context, ChunkMeta_);
    Save(context, ReferencedHunkCount_);
    Save(context, ReferencedTotalHunkLength_);
    Save(context, StoreRefCount_);
    Save(context, PreparedStoreRefCount_);
}

void THunkChunk::Load(TLoadContext& context)
{
    using NYT::Load;
    Load(context, State_);
    Load(context, ChunkMeta_);
    Load(context, ReferencedHunkCount_);
    Load(context, ReferencedTotalHunkLength_);
    Load(context, StoreRefCount_);
    Load(context, PreparedStoreRefCount_);
}

bool THunkChunk::IsDangling() const
{
    return StoreRefCount_ == 0 && PreparedStoreRefCount_ == 0;
}

////////////////////////////////////////////////////////////////////////////////

void THunkChunkIdFormatter::operator()(TStringBuilderBase* builder, const THunkChunkPtr& hunkChunk) const
{
    FormatValue(builder, hunkChunk->GetId(), TStringBuf());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
