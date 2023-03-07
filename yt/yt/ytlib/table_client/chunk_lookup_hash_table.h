#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/block.h>
#include <yt/ytlib/chunk_client/block_cache.h>

#include <yt/core/misc/linear_probe.h>
#include <yt/core/misc/ref.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IChunkLookupHashTable
    : public virtual TRefCounted
{
public:
    virtual void Insert(TKey key, std::pair<ui16, ui32> index) = 0;
    virtual SmallVector<std::pair<ui16, ui32>, 1> Find(TKey key) const = 0;
    virtual size_t GetByteSize() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkLookupHashTable)

////////////////////////////////////////////////////////////////////////////////

IChunkLookupHashTablePtr CreateChunkLookupHashTable(
    int startBlockIndex,
    const std::vector<NChunkClient::TBlock>& blocks,
    const TCachedVersionedChunkMetaPtr& chunkMeta,
    const TKeyComparer& keyComparer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
