#pragma once

#include "public.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/actions/public.h>


namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

struct IUserJobIOFactory
    : public virtual TRefCounted
{
    virtual NTableClient::ISchemalessMultiChunkReaderPtr CreateReader(
        NApi::INativeClientPtr client,
        const NNodeTrackerClient::TNodeDescriptor& nodeDescriptor,
        TClosure onNetworkReleased,
        NTableClient::TNameTablePtr nameTable,
        const NTableClient::TColumnFilter& columnFilter) = 0;

    virtual NTableClient::ISchemalessMultiChunkWriterPtr CreateWriter(
        NApi::INativeClientPtr client,
        NTableClient::TTableWriterConfigPtr config,
        NTableClient::TTableWriterOptionsPtr options,
        const NChunkClient::TChunkListId& chunkListId,
        const NTransactionClient::TTransactionId& transactionId,
        const NTableClient::TTableSchema& tableSchema,
        const NTableClient::TChunkTimestamps& chunkTimestamps) = 0;
};
DEFINE_REFCOUNTED_TYPE(IUserJobIOFactory);

////////////////////////////////////////////////////////////////////////////////

IUserJobIOFactoryPtr CreateUserJobIOFactory(const IJobSpecHelperPtr& jobSpecHelper);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
