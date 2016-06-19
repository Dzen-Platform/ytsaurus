#include "file_chunk_output.h"
#include "private.h"
#include "config.h"
#include "file_chunk_writer.h"

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/config.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/ytlib/chunk_client/confirming_writer.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/helpers.h>

namespace NYT {
namespace NFileClient {

using namespace NYTree;
using namespace NConcurrency;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NChunkClient::NProto;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

TFileChunkOutput::TFileChunkOutput(
    TFileWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    INativeClientPtr client,
    const TTransactionId& transactionId,
    i64 sizeLimit)
    : Config_(config)
    , Options_(options)
    , Client_(client)
    , TransactionId_(transactionId)
    , SizeLimit_(sizeLimit)
    , Logger(FileClientLogger)
{
    YCHECK(Config_);
    YCHECK(Client_);

    auto connection = Client_->GetNativeConnection();
    const auto& secondaryCellTags = connection->GetSecondaryMasterCellTags();
    auto cellTag = secondaryCellTags.empty()
        ? connection->GetPrimaryMasterCellTag()
        : secondaryCellTags[RandomNumber(secondaryCellTags.size())];

    LOG_INFO("File chunk output opened (TransactionId: %v, Account: %v, ReplicationFactor: %v, "
        "UploadReplicationFactor: %v, CellTag: %v)",
        TransactionId_,
        Options_->Account,
        Options_->ReplicationFactor,
        Config_->UploadReplicationFactor,
        cellTag);

    ConfirmingChunkWriter_ = CreateConfirmingWriter(
        Config_,
        Options_,
        cellTag,
        TransactionId_,
        NullChunkListId,
        New<TNodeDirectory>(),
        Client_);

    FileChunkWriter_ = CreateFileChunkWriter(
        Config_,
        New<TEncodingWriterOptions>(),
        ConfirmingChunkWriter_);
}

void TFileChunkOutput::DoWrite(const void* buf, size_t len)
{
    if (GetSize() > SizeLimit_) {
        return;
    }

    if (!FileChunkWriter_->Write(TRef(const_cast<void*>(buf), len))) {
        WaitFor(FileChunkWriter_->GetReadyEvent())
            .ThrowOnError();
    }
}

void TFileChunkOutput::DoFinish()
{
    if (GetSize() > 0) {
        LOG_INFO("Closing file writer");

        WaitFor(FileChunkWriter_->Close())
            .ThrowOnError();
    }

    LOG_INFO("File writer closed");
}

TChunkId TFileChunkOutput::GetChunkId() const
{
    return ConfirmingChunkWriter_->GetChunkId();
}

i64 TFileChunkOutput::GetSize() const
{
    return FileChunkWriter_->GetDataSize();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
