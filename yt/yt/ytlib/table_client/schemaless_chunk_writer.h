#pragma once

#include "public.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/chunk_client/chunk_writer_base.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/multi_chunk_writer.h>
#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/client/table_client/unversioned_writer.h>

#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/rpc/public.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct ISchemalessChunkWriter
    : public IUnversionedWriter
    , public virtual NChunkClient::IChunkWriterBase
{ };

DEFINE_REFCOUNTED_TYPE(ISchemalessChunkWriter)

////////////////////////////////////////////////////////////////////////////////

struct TChunkTimestamps
{
    TChunkTimestamps() = default;
    TChunkTimestamps(TTimestamp minTimestamp, TTimestamp maxTimestamp);

    NTransactionClient::TTimestamp MinTimestamp = NTransactionClient::NullTimestamp;
    NTransactionClient::TTimestamp MaxTimestamp = NTransactionClient::NullTimestamp;
};

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkWriterPtr CreateSchemalessChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TTableSchemaPtr schema,
    NChunkClient::IChunkWriterPtr chunkWriter,
    const TChunkTimestamps& chunkTimestamps = TChunkTimestamps(),
    NChunkClient::IBlockCachePtr blockCache = NChunkClient::GetNullBlockCache());

////////////////////////////////////////////////////////////////////////////////

struct ISchemalessMultiChunkWriter
    : public IUnversionedWriter
    , public virtual NChunkClient::IMultiChunkWriter
{ };

DEFINE_REFCOUNTED_TYPE(ISchemalessMultiChunkWriter)

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkWriterPtr CreateSchemalessMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TNameTablePtr nameTable,
    TTableSchemaPtr schema,
    NTableClient::TLegacyOwningKey lastKey,
    NApi::NNative::IClientPtr client,
    NObjectClient::TCellTag cellTag,
    NTransactionClient::TTransactionId transactionId,
    NChunkClient::TChunkListId parentChunkListId = NChunkClient::NullChunkListId,
    const TChunkTimestamps& chunkTimestamps = TChunkTimestamps(),
    NChunkClient::TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr throttler = NConcurrency::GetUnlimitedThrottler(),
    NChunkClient::IBlockCachePtr blockCache = NChunkClient::GetNullBlockCache());

ISchemalessMultiChunkWriterPtr CreatePartitionMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TNameTablePtr nameTable,
    TTableSchemaPtr schema,
    NApi::NNative::IClientPtr client,
    NObjectClient::TCellTag cellTag,
    NTransactionClient::TTransactionId transactionId,
    NChunkClient::TChunkListId parentChunkListId,
    IPartitionerPtr partitioner,
    NChunkClient::TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr throttler = NConcurrency::GetUnlimitedThrottler(),
    NChunkClient::IBlockCachePtr blockCache = NChunkClient::GetNullBlockCache());

////////////////////////////////////////////////////////////////////////////////

TFuture<IUnversionedWriterPtr> CreateSchemalessTableWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    const NYPath::TRichYPath& richPath,
    TNameTablePtr nameTable,
    NApi::NNative::IClientPtr client,
    NApi::ITransactionPtr transaction,
    NConcurrency::IThroughputThrottlerPtr throttler = NConcurrency::GetUnlimitedThrottler(),
    NChunkClient::IBlockCachePtr blockCache = NChunkClient::GetNullBlockCache());

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
