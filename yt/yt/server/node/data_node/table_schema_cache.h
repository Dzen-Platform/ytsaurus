#pragma once

#include "public.h"

#include <yt/server/node/tablet_node/sorted_dynamic_comparer.h>

#include <yt/client/object_client/public.h>

#include <yt/core/misc/sync_cache.h>

#include <util/datetime/base.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

using TSchemaCacheKey = std::pair<NObjectClient::TObjectId, NHydra::TRevision>;

struct TCachedTableSchema
    : public TIntrinsicRefCounted
{
    TCachedTableSchema(
        NTableClient::TTableSchemaPtr tableSchema,
        NTabletNode::TSortedDynamicRowKeyComparer rowKeyComparer);

    NTableClient::TTableSchemaPtr TableSchema;
    NTabletNode::TSortedDynamicRowKeyComparer RowKeyComparer;
};

DEFINE_REFCOUNTED_TYPE(TCachedTableSchema)

////////////////////////////////////////////////////////////////////////////////

class TCachedTableSchemaWrapper
    : public TSyncCacheValueBase<TSchemaCacheKey, TCachedTableSchemaWrapper>
{
public:
    TCachedTableSchemaWrapper(
        TSchemaCacheKey schemaCacheKey,
        TDuration requestTimeout);

    bool IsSet();

    bool TryRequestSchema();

    TCachedTableSchemaPtr GetValue();

    void SetValue(TCachedTableSchemaPtr cachedTableSchema);

private:
    const TDuration RequestTimeout_;
    std::atomic<TInstant> NextRequestTime_;

    // NB: For concurrent access of CachedTableSchema_.
    NConcurrency::TReaderWriterSpinLock SpinLock_;
    TCachedTableSchemaPtr CachedTableSchema_;

    bool CheckSchemaSet();
};

DEFINE_REFCOUNTED_TYPE(TCachedTableSchemaWrapper)

////////////////////////////////////////////////////////////////////////////////

//! Manages cached table schema and corresponding row comparer.
class TTableSchemaCache
    : public TSyncSlruCacheBase<TSchemaCacheKey, TCachedTableSchemaWrapper>
{
public:
    explicit TTableSchemaCache(const TTableSchemaCacheConfigPtr& config);

    TCachedTableSchemaWrapperPtr GetOrCreate(const TSchemaCacheKey& key);

private:
    const TDuration TableSchemaCacheRequestTimeout_;
};

DEFINE_REFCOUNTED_TYPE(TTableSchemaCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
