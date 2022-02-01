#pragma once

#include "public.h"

#include <yt/yt/ytlib/chunk_client/block.h>
#include <yt/yt/ytlib/chunk_client/block_cache.h>

#include <yt/yt/core/misc/linear_probe.h>
#include <yt/yt/core/misc/ref.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IChunkLookupHashTable
    : public virtual TRefCounted
{
public:
    virtual void Insert(TLegacyKey key, std::pair<ui16, ui32> index) = 0;
    virtual TCompactVector<std::pair<ui16, ui32>, 1> Find(TLegacyKey key) const = 0;
    virtual size_t GetByteSize() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkLookupHashTable)

////////////////////////////////////////////////////////////////////////////////

IChunkLookupHashTablePtr CreateChunkLookupHashTable(
    NChunkClient::TChunkId chunkId,
    int startBlockIndex,
    const std::vector<NChunkClient::TBlock>& blocks,
    const TCachedVersionedChunkMetaPtr& chunkMeta,
    const TTableSchemaPtr& tableSchema,
    const TKeyComparer& keyComparer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
