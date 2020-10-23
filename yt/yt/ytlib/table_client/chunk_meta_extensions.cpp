#include "chunk_meta_extensions.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/core/misc/object_pool.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NTableClient {

using namespace NProto;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::EChunkType;
using NYson::IYsonConsumer;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

REGISTER_PROTO_EXTENSION(TTableSchemaExt, 50, table_schema)
REGISTER_PROTO_EXTENSION(TBlockMetaExt, 51, block_meta)
REGISTER_PROTO_EXTENSION(TNameTableExt, 53, name_table)
REGISTER_PROTO_EXTENSION(TBoundaryKeysExt, 55, boundary_keys)
REGISTER_PROTO_EXTENSION(TSamplesExt, 56, samples)
REGISTER_PROTO_EXTENSION(TPartitionsExt, 59, partitions)
REGISTER_PROTO_EXTENSION(TColumnMetaExt, 58, column_meta)
REGISTER_PROTO_EXTENSION(TColumnarStatisticsExt, 60, columnar_statistics)
REGISTER_PROTO_EXTENSION(THeavyColumnStatisticsExt, 61, heavy_column_statistics)
REGISTER_PROTO_EXTENSION(TKeyColumnsExt, 14, key_columns)

////////////////////////////////////////////////////////////////////////////////

size_t TOwningBoundaryKeys::SpaceUsed() const
{
    return
       sizeof(*this) +
       MinKey.GetSpaceUsed() - sizeof(MinKey) +
       MaxKey.GetSpaceUsed() - sizeof(MaxKey);
}

void TOwningBoundaryKeys::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinKey);
    Persist(context, MaxKey);
}

bool TOwningBoundaryKeys::operator ==(const TOwningBoundaryKeys& other) const
{
    return MinKey == other.MinKey && MaxKey == other.MaxKey;
}

bool TOwningBoundaryKeys::operator !=(const TOwningBoundaryKeys& other) const
{
    return MinKey != other.MinKey || MaxKey != other.MaxKey;
}

TString ToString(const TOwningBoundaryKeys& keys)
{
    return Format("MinKey: %v, MaxKey: %v",
        keys.MinKey,
        keys.MaxKey);
}

void Serialize(const TOwningBoundaryKeys& keys, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("min_key").Value(keys.MinKey)
            .Item("max_key").Value(keys.MaxKey)
        .EndMap();
}

void Deserialize(TOwningBoundaryKeys& keys, const INodePtr& node)
{
    const auto& mapNode = node->AsMap();
    // Boundary keys for empty tables look like {} in YSON.
    if (mapNode->GetChildCount() == 0) {
        keys.MinKey = TUnversionedOwningRow();
        keys.MaxKey = TUnversionedOwningRow();
        return;
    }
    Deserialize(keys.MinKey, mapNode->GetChildOrThrow("min_key"));
    Deserialize(keys.MaxKey, mapNode->GetChildOrThrow("max_key"));
}

////////////////////////////////////////////////////////////////////////////////

bool FindBoundaryKeys(const TChunkMeta& chunkMeta, TLegacyOwningKey* minKey, TLegacyOwningKey* maxKey)
{
    auto boundaryKeys = FindProtoExtension<TBoundaryKeysExt>(chunkMeta.extensions());
    if (!boundaryKeys) {
        return false;
    }
    FromProto(minKey, boundaryKeys->min());
    FromProto(maxKey, boundaryKeys->max());
    return true;
}

std::unique_ptr<TOwningBoundaryKeys> FindBoundaryKeys(const TChunkMeta& chunkMeta)
{
    TOwningBoundaryKeys keys;
    if (!FindBoundaryKeys(chunkMeta, &keys.MinKey, &keys.MaxKey)) {
        return nullptr;
    }
    return std::make_unique<TOwningBoundaryKeys>(std::move(keys));
}

TChunkMeta FilterChunkMetaByPartitionTag(const TChunkMeta& chunkMeta, const TCachedBlockMetaPtr& cachedBlockMeta, int partitionTag)
{
    YT_VERIFY(chunkMeta.type() == static_cast<int>(EChunkType::Table));
    auto filteredChunkMeta = chunkMeta;

    std::vector<TBlockMeta> filteredBlocks;
    for (const auto& blockMeta : cachedBlockMeta->blocks()) {
        YT_VERIFY(blockMeta.partition_index() != DefaultPartitionTag);
        if (blockMeta.partition_index() == partitionTag) {
            filteredBlocks.push_back(blockMeta);
        }
    }

    auto blockMetaExt = ObjectPool<TBlockMetaExt>().Allocate();
    NYT::ToProto(blockMetaExt->mutable_blocks(), filteredBlocks);
    SetProtoExtension(filteredChunkMeta.mutable_extensions(), *blockMetaExt);

    return filteredChunkMeta;
}

////////////////////////////////////////////////////////////////////////////////

TCachedBlockMeta::TCachedBlockMeta(NChunkClient::TChunkId chunkId, NTableClient::NProto::TBlockMetaExt blockMeta)
    : TSyncCacheValueBase<NChunkClient::TChunkId, TCachedBlockMeta>(chunkId)
    , NTableClient::NProto::TBlockMetaExt(std::move(blockMeta))
{
    Weight_ = SpaceUsedLong();
}

i64 TCachedBlockMeta::GetWeight() const
{
    return Weight_;
}

////////////////////////////////////////////////////////////////////////////////

TBlockMetaCache::TBlockMetaCache(TSlruCacheConfigPtr config, const NProfiling::TProfiler& profiler)
    : TSyncSlruCacheBase<NChunkClient::TChunkId, TCachedBlockMeta>(std::move(config), profiler)
{ }

i64 TBlockMetaCache::GetWeight(const TCachedBlockMetaPtr& value) const
{
    return value->GetWeight();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
