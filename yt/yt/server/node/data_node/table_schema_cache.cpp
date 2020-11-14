#include "table_schema_cache.h"

#include "private.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/server/node/data_node/config.h>

#include <yt/server/node/tablet_node/sorted_dynamic_comparer.h>

#include <yt/client/table_client/schema.h>

#include <yt/core/misc/sync_cache.h>

namespace NYT::NDataNode {

using namespace NObjectClient;
using namespace NProfiling;
using namespace NHydra;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TCachedTableSchema::TCachedTableSchema(
    NTableClient::TTableSchemaPtr tableSchema,
    NTabletNode::TSortedDynamicRowKeyComparer rowKeyComparer)
    : TableSchema(std::move(tableSchema))
    , RowKeyComparer(std::move(rowKeyComparer))
{ }

////////////////////////////////////////////////////////////////////////////////

using TSchemaCacheKey = std::pair<TObjectId, TRevision>;

TCachedTableSchemaWrapper::TCachedTableSchemaWrapper(
    TSchemaCacheKey schemaCacheKey,
    i64 schemaSize,
    TDuration requestTimeout)
    : TSyncCacheValueBase(std::move(schemaCacheKey))
    , RequestTimeout_(requestTimeout)
    , SchemaSize_(schemaSize)
    , NextRequestTime_(NProfiling::GetInstant())
{ 
    YT_VERIFY(SchemaSize_ > 0);
}

bool TCachedTableSchemaWrapper::IsSet()
{
    TReaderGuard readerGuard(SpinLock_);
    return CheckSchemaSet();
}

bool TCachedTableSchemaWrapper::TryRequestSchema()
{
    if (IsSet()) {
        return false;
    }

    auto allowedRequestTime = NextRequestTime_.load();
    auto curTime = GetInstant();
    if (curTime < allowedRequestTime) {
        return false;
    }

    return NextRequestTime_.compare_exchange_strong(allowedRequestTime, curTime + RequestTimeout_);
}

TCachedTableSchemaPtr TCachedTableSchemaWrapper::GetValue()
{
    TReaderGuard guard(SpinLock_);
    YT_VERIFY(CheckSchemaSet());
    return CachedTableSchema_;
}

void TCachedTableSchemaWrapper::SetValue(TCachedTableSchemaPtr cachedTableSchema)
{
    TWriterGuard guard(SpinLock_);
    if (CheckSchemaSet()) {
        YT_VERIFY(*CachedTableSchema_->TableSchema == *cachedTableSchema->TableSchema);
        return;
    }

    CachedTableSchema_ = std::move(cachedTableSchema);
}

i64 TCachedTableSchemaWrapper::GetWeight() const
{
    return SchemaSize_;
}

bool TCachedTableSchemaWrapper::CheckSchemaSet()
{
    VERIFY_SPINLOCK_AFFINITY(SpinLock_);

    return static_cast<bool>(CachedTableSchema_);
}

////////////////////////////////////////////////////////////////////////////////

TTableSchemaCache::TTableSchemaCache(const TTableSchemaCacheConfigPtr& config)
    : TSyncSlruCacheBase(config, TRegistry("/data_node/table_schema_cache"))
    , TableSchemaCacheRequestTimeout_(config->TableSchemaCacheRequestTimeout)
{ }

TCachedTableSchemaWrapperPtr TTableSchemaCache::GetOrCreate(const TSchemaCacheKey& key, i64 schemaSize)
{
    if (auto result = Find(key)) {
        return result;
    }

    auto emptyTableSchema = New<TCachedTableSchemaWrapper>(key, schemaSize, TableSchemaCacheRequestTimeout_);
    TCachedTableSchemaWrapperPtr existingTableSchema;
    if (!TryInsert(emptyTableSchema, &existingTableSchema)) {
        return existingTableSchema;
    }

    return emptyTableSchema;
}

i64 TTableSchemaCache::GetWeight(const TCachedTableSchemaWrapperPtr& value) const
{
    return value->GetWeight();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
