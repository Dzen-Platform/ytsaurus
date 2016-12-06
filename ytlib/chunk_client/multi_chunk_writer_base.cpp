#include "multi_chunk_writer_base.h"
#include "private.h"
#include "chunk_replica.h"
#include "chunk_writer.h"
#include "config.h"
#include "confirming_writer.h"
#include "dispatcher.h"

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/config.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/chunk_client/chunk_spec.pb.h>
#include <yt/ytlib/chunk_client/chunk_writer_base.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/address.h>

#include <yt/core/rpc/channel.h>
#include <yt/core/rpc/helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NErasure;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NTransactionClient;
using namespace NObjectClient;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TNontemplateMultiChunkWriterBase::TNontemplateMultiChunkWriterBase(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    INativeClientPtr client,
    TCellTag cellTag,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
    : Logger(ChunkClientLogger)
    , Client_(client)
    , Config_(NYTree::CloneYsonSerializable(config))
    , Options_(options)
    , CellTag_(cellTag)
    , TransactionId_(transactionId)
    , ParentChunkListId_(parentChunkListId)
    , Throttler_(throttler)
    , BlockCache_(blockCache)
    , NodeDirectory_(New<TNodeDirectory>())
{
    YCHECK(Config_);

    Config_->UploadReplicationFactor = std::min(
        Options_->ReplicationFactor,
        Config_->UploadReplicationFactor);

    Logger.AddTag("TransactionId: %v", TransactionId_);
}

TFuture<void> TNontemplateMultiChunkWriterBase::Open()
{
    ReadyEvent_= BIND(&TNontemplateMultiChunkWriterBase::InitSession, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();

    return ReadyEvent_;
}

TFuture<void> TNontemplateMultiChunkWriterBase::Close()
{
    YCHECK(!Closing_);
    YCHECK(ReadyEvent_.IsSet() && ReadyEvent_.Get().IsOK());

    Closing_ = true;
    ReadyEvent_ = BIND(&TNontemplateMultiChunkWriterBase::FinishSession, MakeWeak(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();

    return ReadyEvent_;
}

TFuture<void> TNontemplateMultiChunkWriterBase::GetReadyEvent()
{
    if (SwitchingSession_) {
        return ReadyEvent_;
    } else {
        return CurrentSession_.TemplateWriter->GetReadyEvent();
    }
}

void TNontemplateMultiChunkWriterBase::SetProgress(double progress)
{
    Progress_ = progress;
}

const std::vector<TChunkSpec>& TNontemplateMultiChunkWriterBase::GetWrittenChunksMasterMeta() const
{
    return WrittenChunks_;
}

const std::vector<TChunkSpec>& TNontemplateMultiChunkWriterBase::GetWrittenChunksFullMeta() const
{
    return WrittenChunksFullMeta_;
}

TNodeDirectoryPtr TNontemplateMultiChunkWriterBase::GetNodeDirectory() const
{
    return NodeDirectory_;
}

TDataStatistics TNontemplateMultiChunkWriterBase::GetDataStatistics() const
{
    TGuard<TSpinLock> guard(SpinLock_);
    auto result = DataStatistics_;
    if (CurrentSession_.IsActive()) {
        result += CurrentSession_.TemplateWriter->GetDataStatistics();
    }
    return result;
}

void TNontemplateMultiChunkWriterBase::SwitchSession()
{
    SwitchingSession_ = true;
    ReadyEvent_ = BIND(
        &TNontemplateMultiChunkWriterBase::DoSwitchSession,
        MakeWeak(this))
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();
}

void TNontemplateMultiChunkWriterBase::DoSwitchSession()
{
    FinishSession();
    InitSession();
}

void TNontemplateMultiChunkWriterBase::FinishSession()
{
    if (CurrentSession_.TemplateWriter->GetDataSize() == 0) {
        return;
    }

    WaitFor(CurrentSession_.TemplateWriter->Close())
        .ThrowOnError();

    TChunkSpec chunkSpec;
    *chunkSpec.mutable_chunk_meta() = CurrentSession_.TemplateWriter->GetSchedulerMeta();
    ToProto(chunkSpec.mutable_chunk_id(), CurrentSession_.UnderlyingWriter->GetChunkId());
    NYT::ToProto(chunkSpec.mutable_replicas(), CurrentSession_.UnderlyingWriter->GetWrittenChunkReplicas());

    WrittenChunks_.push_back(chunkSpec);

    *chunkSpec.mutable_chunk_meta() = CurrentSession_.TemplateWriter->GetNodeMeta();
    WrittenChunksFullMeta_.push_back(chunkSpec);

    TGuard<TSpinLock> guard(SpinLock_);
    DataStatistics_ += CurrentSession_.TemplateWriter->GetDataStatistics();
    CurrentSession_.Reset();
}

void TNontemplateMultiChunkWriterBase::InitSession()
{
    CurrentSession_.UnderlyingWriter = CreateConfirmingWriter(
        Config_,
        Options_,
        CellTag_,
        TransactionId_,
        ParentChunkListId_,
        NodeDirectory_,
        Client_,
        BlockCache_,
        Throttler_);

    CurrentSession_.TemplateWriter = CreateTemplateWriter(CurrentSession_.UnderlyingWriter);
    WaitFor(CurrentSession_.TemplateWriter->Open())
        .ThrowOnError();

    SwitchingSession_ = false;
}

bool TNontemplateMultiChunkWriterBase::TrySwitchSession()
{
    if (CurrentSession_.TemplateWriter->IsCloseDemanded()) {
        LOG_DEBUG("Switching to next chunk due to chunk writer demand");

        SwitchSession();
        return true;
    }

    if (CurrentSession_.TemplateWriter->GetMetaSize() > Config_->MaxMetaSize) {
        LOG_DEBUG("Switching to next chunk: meta is too large (ChunkMetaSize: %v)",
            CurrentSession_.TemplateWriter->GetMetaSize());

        SwitchSession();
        return true;
    }

    if (CurrentSession_.TemplateWriter->GetDataSize() > Config_->DesiredChunkSize) {
        i64 currentDataSize = DataStatistics_.compressed_data_size() + CurrentSession_.TemplateWriter->GetDataSize();
        i64 expectedInputSize = static_cast<i64>(currentDataSize * std::max(0.0, 1.0 - Progress_));

        if (expectedInputSize > Config_->DesiredChunkSize ||
            // On erasure chunks switch immediately, otherwise we can consume too much memory.
            Options_->ErasureCodec != ECodec::None ||
            CurrentSession_.TemplateWriter->GetDataSize() > 2 * Config_->DesiredChunkSize)
        {
            LOG_DEBUG("Switching to next chunk: data is too large (CurrentSessionSize: %v, ExpectedInputSize: %v, DesiredChunkSize: %v)",
                CurrentSession_.TemplateWriter->GetDataSize(),
                expectedInputSize,
                Config_->DesiredChunkSize);

            SwitchSession();
            return true;
        }
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
