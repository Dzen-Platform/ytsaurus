#include "replication_writer.h"
#include "private.h"
#include "block_cache.h"
#include "chunk_meta_extensions.h"
#include "chunk_replica.h"
#include "chunk_service_proxy.h"
#include "chunk_writer.h"
#include "config.h"
#include "data_node_service_proxy.h"
#include "dispatcher.h"
#include "helpers.h"

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/config.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/rpc/retrying_channel.h>

#include <yt/core/misc/async_stream_state.h>
#include <yt/core/misc/nullable.h>

#include <atomic>
#include <deque>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NRpc;
using namespace NApi;
using namespace NObjectClient;

///////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TReplicationWriter);
DECLARE_REFCOUNTED_STRUCT(TNode);
DECLARE_REFCOUNTED_CLASS(TGroup);

///////////////////////////////////////////////////////////////////////////////

struct TNode
    : public TRefCounted
{
    const int Index;
    const TNodeDescriptor Descriptor;
    const TChunkReplica ChunkReplica;
    const IChannelPtr LightChannel;
    const IChannelPtr HeavyChannel;

    TError Error;
    TPeriodicExecutorPtr PingExecutor;
    std::atomic_flag Canceled = ATOMIC_FLAG_INIT;

    TNode(
        int index,
        const TNodeDescriptor& descriptor,
        TChunkReplica chunkReplica,
        IChannelPtr lightChannel,
        IChannelPtr heavyChannel)
        : Index(index)
        , Descriptor(descriptor)
        , ChunkReplica(chunkReplica)
        , LightChannel(std::move(lightChannel))
        , HeavyChannel(std::move(heavyChannel))
    { }

    bool IsAlive() const
    {
        return Error.IsOK();
    }

};

Stroka ToString(const TNodePtr& node)
{
    return node->Descriptor.GetDefaultAddress();
}

DEFINE_REFCOUNTED_TYPE(TNode);

///////////////////////////////////////////////////////////////////////////////

class TGroup
    : public TRefCounted
{
public:
    TGroup(
        TReplicationWriter* writer,
        int startBlockIndex);

    void AddBlock(const TSharedRef& block);

    void ScheduleProcess();

    void SetFlushing();

    bool IsWritten() const;

    bool IsFlushing() const;

    i64 GetSize() const;

    int GetStartBlockIndex() const;

    int GetEndBlockIndex() const;

private:
    bool Flushing_ = false;
    std::vector<bool> SentTo_;

    std::vector<TSharedRef> Blocks_;
    int FirstBlockIndex_;

    i64 Size_ = 0;

    TWeakPtr<TReplicationWriter> Writer_;
    NLogging::TLogger Logger;

    void PutGroup(TReplicationWriterPtr writer);

    void SendGroup(TReplicationWriterPtr writer, TNodePtr srcNode);

    void Process();

};

DEFINE_REFCOUNTED_TYPE(TGroup);

///////////////////////////////////////////////////////////////////////////////

class TReplicationWriter
    : public IChunkWriter
{
public:
    TReplicationWriter(
        TReplicationWriterConfigPtr config,
        TRemoteWriterOptionsPtr options,
        const TChunkId& chunkId,
        const TChunkReplicaList& initialTargets,
        TNodeDirectoryPtr nodeDirectory,
        INativeClientPtr client,
        IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache);

    ~TReplicationWriter();

    virtual TFuture<void> Open() override;

    virtual bool WriteBlock(const TSharedRef& block) override;
    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual TFuture<void> Close(const TChunkMeta& chunkMeta) override;

    virtual const TChunkInfo& GetChunkInfo() const override;
    virtual const TDataStatistics& GetDataStatistics() const override;
    virtual TChunkReplicaList GetWrittenChunkReplicas() const override;

    virtual TChunkId GetChunkId() const override;
    virtual NErasure::ECodec GetErasureCodecId() const override;

private:
    friend class TGroup;

    const TReplicationWriterConfigPtr Config_;
    const TRemoteWriterOptionsPtr Options_;
    const TChunkId ChunkId_;
    const TChunkReplicaList InitialTargets_;
    const INativeClientPtr Client_;
    const TNodeDirectoryPtr NodeDirectory_;
    const IThroughputThrottlerPtr Throttler_;
    const IBlockCachePtr BlockCache_;

    Stroka NetworkName_;

    TAsyncStreamState State_;

    bool IsOpen_ = false;
    bool IsClosing_ = false;

    //! This flag is raised whenever #Close is invoked.
    //! All access to this flag happens from #WriterThread.
    bool IsCloseRequested_ = false;
    TChunkMeta ChunkMeta_;

    std::deque<TGroupPtr> Window_;
    TAsyncSemaphorePtr WindowSlots_;

    std::vector<TNodePtr> Nodes_;

    //! Number of nodes that are still alive.
    int AliveNodeCount_ = 0;

    const int UploadReplicationFactor_;
    const int MinUploadReplicationFactor_;

    //! A new group of blocks that is currently being filled in by the client.
    //! All access to this field happens from client thread.
    TGroupPtr CurrentGroup_;

    //! Number of blocks that are already added via #AddBlocks.
    int BlockCount_ = 0;

    //! Returned from node on Finish.
    TChunkInfo ChunkInfo_;

    //! Last time write targets were allocated from the master.
    TInstant AllocateWriteTargetsTimestamp_;

    int AllocateWriteTargetsRetryIndex_ = 0;

    NLogging::TLogger Logger = ChunkClientLogger;


    void DoOpen();
    void DoClose();

    TChunkReplicaList AllocateTargets();
    void StartSessions(const TChunkReplicaList& targets);

    void EnsureCurrentGroup();
    void FlushCurrentGroup();

    void OnNodeFailed(TNodePtr node, const TError& error);

    void ShiftWindow();
    void OnWindowShifted(int blockIndex, const TError& error);

    void FlushBlocks(TNodePtr node, int blockIndex);

    void StartChunk(TChunkReplica target);

    void CloseSessions();

    void FinishChunk(TNodePtr node);

    void SendPing(const TWeakPtr<TNode>& node);

    void CancelWriter(bool abort);
    void CancelNode(TNodePtr node, bool abort);

    void AddBlocks(const std::vector<TSharedRef>& blocks);

    IChannelPtr CreateRetryingNodeChannel(
        IChannelFactoryPtr channelFactory,
        const Stroka& address);

    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);
};

DEFINE_REFCOUNTED_TYPE(TReplicationWriter);

///////////////////////////////////////////////////////////////////////////////

TGroup::TGroup(
    TReplicationWriter* writer,
    int startBlockIndex)
    : SentTo_(writer->Nodes_.size(), false)
    , FirstBlockIndex_(startBlockIndex)
    , Writer_(writer)
    , Logger(writer->Logger)
{ }

void TGroup::AddBlock(const TSharedRef& block)
{
    Blocks_.push_back(block);
    Size_ += block.Size();
}

int TGroup::GetStartBlockIndex() const
{
    return FirstBlockIndex_;
}

int TGroup::GetEndBlockIndex() const
{
    return FirstBlockIndex_ + Blocks_.size() - 1;
}

i64 TGroup::GetSize() const
{
    return Size_;
}

bool TGroup::IsWritten() const
{
    auto writer = Writer_.Lock();
    YCHECK(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    for (int nodeIndex = 0; nodeIndex < SentTo_.size(); ++nodeIndex) {
        if (writer->Nodes_[nodeIndex]->IsAlive() && !SentTo_[nodeIndex]) {
            return false;
        }
    }
    return true;
}

void TGroup::PutGroup(TReplicationWriterPtr writer)
{
    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    int nodeIndex = 0;
    while (!writer->Nodes_[nodeIndex]->IsAlive()) {
        ++nodeIndex;
        YCHECK(nodeIndex < writer->Nodes_.size());
    }

    auto node = writer->Nodes_[nodeIndex];

    TDataNodeServiceProxy proxy(node->HeavyChannel);
    auto req = proxy.PutBlocks();
    req->SetTimeout(writer->Config_->NodeRpcTimeout);
    ToProto(req->mutable_chunk_id(), writer->ChunkId_);
    req->set_first_block_index(FirstBlockIndex_);
    req->set_populate_cache(writer->Config_->PopulateCache);
    req->Attachments().insert(req->Attachments().begin(), Blocks_.begin(), Blocks_.end());

    LOG_DEBUG("Ready to put blocks (Blocks: %v-%v, Address: %v, Size: %v)",
        GetStartBlockIndex(),
        GetEndBlockIndex(),
        node->Descriptor.GetDefaultAddress(),
        Size_);

    WaitFor(writer->Throttler_->Throttle(Size_));

    LOG_DEBUG("Putting blocks (Blocks: %v-%v, Address: %v)",
        FirstBlockIndex_,
        GetEndBlockIndex(),
        node->Descriptor.GetDefaultAddress());

    auto rspOrError = WaitFor(req->Invoke());
    if (rspOrError.IsOK()) {
        SentTo_[node->Index] = true;

        LOG_DEBUG("Blocks are put (Blocks: %v-%v, Address: %v)",
            GetStartBlockIndex(),
            GetEndBlockIndex(),
            node->Descriptor.GetDefaultAddress());
    } else {
        writer->OnNodeFailed(node, rspOrError);
    }

    ScheduleProcess();
}

void TGroup::SendGroup(TReplicationWriterPtr writer, TNodePtr srcNode)
{
    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    TNodePtr dstNode;
    for (int index = 0; index < SentTo_.size(); ++index) {
        auto node = writer->Nodes_[index];
        if (node->IsAlive() && !SentTo_[index]) {
            dstNode = node;
        }
    }

    if (dstNode) {
        LOG_DEBUG("Sending blocks (Blocks: %v-%v, SrcAddress: %v, DstAddress: %v)",
            GetStartBlockIndex(),
            GetEndBlockIndex(),
            srcNode->Descriptor.GetDefaultAddress(),
            dstNode->Descriptor.GetDefaultAddress());

        TDataNodeServiceProxy proxy(srcNode->LightChannel);
        auto req = proxy.SendBlocks();
        // Set double timeout for SendBlocks since executing it implies another (src->dst) RPC call.
        req->SetTimeout(writer->Config_->NodeRpcTimeout * 2);
        ToProto(req->mutable_chunk_id(), writer->ChunkId_);
        req->set_first_block_index(FirstBlockIndex_);
        req->set_block_count(Blocks_.size());
        ToProto(req->mutable_target_descriptor(), dstNode->Descriptor);

        auto rspOrError = WaitFor(req->Invoke());
        if (rspOrError.IsOK()) {
            LOG_DEBUG("Blocks are sent (Blocks: %v-%v, SrcAddress: %v, DstAddress: %v)",
                FirstBlockIndex_,
                GetEndBlockIndex(),
                srcNode->Descriptor.GetDefaultAddress(),
                dstNode->Descriptor.GetDefaultAddress());
            SentTo_[dstNode->Index] = true;
        } else {
            auto failedNode = (rspOrError.GetCode() == EErrorCode::SendBlocksFailed) ? dstNode : srcNode;
            writer->OnNodeFailed(failedNode, rspOrError);
        }
    }

    ScheduleProcess();
}

bool TGroup::IsFlushing() const
{
    auto writer = Writer_.Lock();
    YCHECK(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    return Flushing_;
}

void TGroup::SetFlushing()
{
    auto writer = Writer_.Lock();
    YCHECK(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    Flushing_ = true;
}

void TGroup::ScheduleProcess()
{
    TDispatcher::Get()->GetWriterInvoker()->Invoke(
        BIND(&TGroup::Process, MakeWeak(this)));
}

void TGroup::Process()
{
    auto writer = Writer_.Lock();
    if (!writer || !writer->State_.IsActive()) {
        return;
    }

    VERIFY_THREAD_AFFINITY(writer->WriterThread);
    YCHECK(writer->IsOpen_);

    LOG_DEBUG("Processing blocks (Blocks: %v-%v)",
        FirstBlockIndex_,
        GetEndBlockIndex());

    TNodePtr nodeWithBlocks;
    bool emptyNodeFound = false;
    for (int nodeIndex = 0; nodeIndex < SentTo_.size(); ++nodeIndex) {
        auto node = writer->Nodes_[nodeIndex];
        if (node->IsAlive()) {
            if (SentTo_[nodeIndex]) {
                nodeWithBlocks = node;
            } else {
                emptyNodeFound = true;
            }
        }
    }

    if (!emptyNodeFound) {
        writer->ShiftWindow();
    } else if (!nodeWithBlocks) {
        PutGroup(writer);
    } else {
        SendGroup(writer, nodeWithBlocks);
    }
}

///////////////////////////////////////////////////////////////////////////////

TReplicationWriter::TReplicationWriter(
    TReplicationWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TChunkId& chunkId,
    const TChunkReplicaList& initialTargets,
    TNodeDirectoryPtr nodeDirectory,
    INativeClientPtr client,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
    : Config_(config)
    , Options_(options)
    , ChunkId_(chunkId)
    , InitialTargets_(initialTargets)
    , Client_(client)
    , NodeDirectory_(nodeDirectory)
    , Throttler_(throttler)
    , BlockCache_(blockCache)
    , NetworkName_(client->GetNativeConnection()->GetConfig()->NetworkName)
    , WindowSlots_(New<TAsyncSemaphore>(config->SendWindowSize))
    , UploadReplicationFactor_(Config_->UploadReplicationFactor)
    , MinUploadReplicationFactor_(std::min(Config_->UploadReplicationFactor, Config_->MinUploadReplicationFactor))
    , AllocateWriteTargetsTimestamp_(TInstant::Zero())
{
    Logger.AddTag("ChunkId: %v, ChunkWriter: %v", ChunkId_, this);
}

TReplicationWriter::~TReplicationWriter()
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Just a quick check.
    if (State_.IsClosed()) {
        return;
    }

    LOG_INFO("Writer canceled");
    State_.Fail(TError("Writer canceled"));

    CancelWriter(true);
}

TChunkReplicaList TReplicationWriter::AllocateTargets()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!Options_->AllowAllocatingNewTargetNodes) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::MasterCommunicationFailed,
            "Allocating new target nodes is disabled");
    }

    ++AllocateWriteTargetsRetryIndex_;
    if (AllocateWriteTargetsRetryIndex_ > Config_->AllocateWriteTargetsRetryCount) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::MasterCommunicationFailed,
            "Failed to allocate write targets, retry count limit exceeded")
            << TErrorAttribute("retry_count", Config_->AllocateWriteTargetsRetryCount);
    }

    auto delayTime = TInstant::Now() - AllocateWriteTargetsTimestamp_;
    if (delayTime < Config_->AllocateWriteTargetsBackoffTime) {
        WaitFor(TDelayedExecutor::MakeDelayed(Config_->AllocateWriteTargetsBackoffTime - delayTime))
            .ThrowOnError();
    }
    AllocateWriteTargetsTimestamp_ = TInstant::Now();

    int activeTargets = Nodes_.size();
    std::vector<Stroka> forbiddenAddresses;
    for (const auto& node : Nodes_) {
        forbiddenAddresses.push_back(node->Descriptor.GetDefaultAddress());
    }

    return AllocateWriteTargets(
        Client_,
        ChunkId_,
        UploadReplicationFactor_ - activeTargets,
        std::max(MinUploadReplicationFactor_ - activeTargets, 1),
        UploadReplicationFactor_,
        Config_->PreferLocalHost,
        forbiddenAddresses,
        NodeDirectory_,
        Logger);
}

void TReplicationWriter::StartSessions(const TChunkReplicaList& targets)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    std::vector<TFuture<void>> asyncResults;
    for (auto target : targets) {
        asyncResults.push_back(
            BIND(&TReplicationWriter::StartChunk, MakeWeak(this), target)
                .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
                .Run());
    }

    WaitFor(Combine(asyncResults))
        .ThrowOnError();
}

void TReplicationWriter::StartChunk(TChunkReplica target)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    auto nodeDescriptor = NodeDirectory_->GetDescriptor(target);
    auto address = nodeDescriptor.GetAddressOrThrow(NetworkName_);
    LOG_DEBUG("Starting write session (Address: %v)", address);

    auto lightChannel = CreateRetryingNodeChannel(
        Client_->GetNodeChannelFactory(),
        address);
    auto heavyChannel = CreateRetryingNodeChannel(
        Client_->GetHeavyChannelFactory(),
        address);

    TDataNodeServiceProxy proxy(lightChannel);
    auto req = proxy.StartChunk();
    req->SetTimeout(Config_->NodeRpcTimeout);
    ToProto(req->mutable_chunk_id(), ChunkId_);
    ToProto(req->mutable_workload_descriptor(), Config_->WorkloadDescriptor);
    req->set_sync_on_close(Config_->SyncOnClose);
    req->set_enable_uniform_placement(Config_->EnableUniformPlacement);

    auto rspOrError = WaitFor(req->Invoke());
    if (!rspOrError.IsOK()) {
        LOG_WARNING(rspOrError, "Failed to start write session on node %v", address);
        return;
    }

    LOG_DEBUG("Write session started (Address: %v)", address);

    auto node = New<TNode>(
        Nodes_.size(),
        nodeDescriptor,
        target,
        lightChannel,
        heavyChannel);

    node->PingExecutor = New<TPeriodicExecutor>(
        TDispatcher::Get()->GetWriterInvoker(),
        BIND(&TReplicationWriter::SendPing, MakeWeak(this), MakeWeak(node)),
        Config_->NodePingPeriod);
    node->PingExecutor->Start();

    Nodes_.push_back(node);
    ++AliveNodeCount_;
}

TFuture<void> TReplicationWriter::Open()
{
    return BIND(&TReplicationWriter::DoOpen, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

void TReplicationWriter::DoOpen()
{
    try {
        StartSessions(InitialTargets_);

        while (Nodes_.size() < UploadReplicationFactor_) {
            StartSessions(AllocateTargets());
        }

        LOG_INFO("Writer opened (Addresses: %v, PopulateCache: %v, Workload: %v, Network: %v)",
            Nodes_,
            Config_->PopulateCache,
            Config_->WorkloadDescriptor,
            NetworkName_);

        IsOpen_ = true;
    } catch (const std::exception& ex) {
        CancelWriter(true);
        THROW_ERROR_EXCEPTION("Not enough target nodes to write blob chunk")
            << TErrorAttribute("chunk_id", ChunkId_)
            << TErrorAttribute("upload_replication_factor", UploadReplicationFactor_)
            << ex;
    }
}

void TReplicationWriter::ShiftWindow()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!State_.IsActive()) {
        YCHECK(Window_.empty());
        return;
    }

    int lastFlushableBlock = -1;
    for (auto it = Window_.begin(); it != Window_.end(); ++it) {
        auto group = *it;
        if (!group->IsFlushing()) {
            if (group->IsWritten()) {
                lastFlushableBlock = group->GetEndBlockIndex();
                group->SetFlushing();
            } else {
                break;
            }
        }
    }

    if (lastFlushableBlock < 0) {
        return;
    }

    std::vector<TFuture<void>> asyncResults;
    for (auto node : Nodes_) {
        asyncResults.push_back(
            BIND(&TReplicationWriter::FlushBlocks, MakeWeak(this), node,lastFlushableBlock)
                .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
                .Run());
    }
    Combine(asyncResults).Subscribe(
        BIND(
            &TReplicationWriter::OnWindowShifted,
            MakeWeak(this),
            lastFlushableBlock)
        .Via(TDispatcher::Get()->GetWriterInvoker()));
}

void TReplicationWriter::OnWindowShifted(int lastFlushedBlock, const TError& error)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!error.IsOK()) {
        LOG_WARNING(error, "Chunk writer failed");
        CancelWriter(true);
        State_.Fail(error);
        return;
    }

    if (Window_.empty()) {
        // This happens when FlushBlocks responses are reordered
        // (i.e. a larger BlockIndex is flushed before a smaller one)
        // We should prevent repeated calls to CloseSessions.
        return;
    }

    while (!Window_.empty()) {
        auto group = Window_.front();
        if (group->GetEndBlockIndex() > lastFlushedBlock) {
            return;
        }

        LOG_DEBUG("Window shifted (Blocks: %v-%v, Size: %v)",
            group->GetStartBlockIndex(),
            group->GetEndBlockIndex(),
            group->GetSize());

        WindowSlots_->Release(group->GetSize());
        Window_.pop_front();
    }

    if (State_.IsActive() && IsCloseRequested_) {
        CloseSessions();
    }
}

void TReplicationWriter::FlushBlocks(TNodePtr node, int blockIndex)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!node->IsAlive()) {
        return;
    }

    LOG_DEBUG("Flushing block (Block: %v, Address: %v)",
        blockIndex,
        node->Descriptor.GetDefaultAddress());

    TDataNodeServiceProxy proxy(node->LightChannel);
    auto req = proxy.FlushBlocks();
    req->SetTimeout(Config_->NodeRpcTimeout);
    ToProto(req->mutable_chunk_id(), ChunkId_);
    req->set_block_index(blockIndex);

    auto rspOrError = WaitFor(req->Invoke());
    if (rspOrError.IsOK()) {
        LOG_DEBUG("Block flushed (Block: %v, Address: %v)",
            blockIndex,
            node->Descriptor.GetDefaultAddress());
    } else {
        OnNodeFailed(node, rspOrError);
    }
}

void TReplicationWriter::EnsureCurrentGroup()
{
    if (!CurrentGroup_) {
        CurrentGroup_ = New<TGroup>(this, BlockCount_);
    }
}

void TReplicationWriter::FlushCurrentGroup()
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YCHECK(!IsCloseRequested_);

    if (!State_.IsActive()) {
        return;
    }

    LOG_DEBUG("Block group added (Blocks: %v-%v, Group: %p)",
        CurrentGroup_->GetStartBlockIndex(),
        CurrentGroup_->GetEndBlockIndex(),
        CurrentGroup_.Get());

    Window_.push_back(CurrentGroup_);
    CurrentGroup_->ScheduleProcess();
    CurrentGroup_.Reset();
}

void TReplicationWriter::OnNodeFailed(TNodePtr node, const TError& error)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!node->IsAlive()) {
        return;
    }

    auto wrappedError = TError("Node %v failed",
        node->Descriptor.GetDefaultAddress())
        << error;
    LOG_ERROR(wrappedError);

    node->Error = wrappedError;
    --AliveNodeCount_;

    if (State_.IsActive() && AliveNodeCount_ < MinUploadReplicationFactor_) {
        auto cumulativeError = TError(
            NChunkClient::EErrorCode::AllTargetNodesFailed,
            "Not enough target nodes to finish upload");
        for (auto node : Nodes_) {
            if (!node->IsAlive()) {
                cumulativeError.InnerErrors().push_back(node->Error);
            }
        }
        LOG_WARNING(cumulativeError, "Chunk writer failed");
        CancelWriter(true);
        State_.Fail(cumulativeError);
    }
}

void TReplicationWriter::CloseSessions()
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YCHECK(IsCloseRequested_);

    LOG_INFO("Closing writer");

    std::vector<TFuture<void>> asyncResults;
    for (auto node : Nodes_) {
        asyncResults.push_back(
            BIND(&TReplicationWriter::FinishChunk, MakeWeak(this), node)
                .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
                .Run());
    }

    WaitFor(Combine(asyncResults))
        .ThrowOnError();

    YCHECK(Window_.empty());

    if (State_.IsActive()) {
        State_.Close();
    }

    CancelWriter(false);

    LOG_INFO("Writer closed");

    State_.FinishOperation();
}

void TReplicationWriter::FinishChunk(TNodePtr node)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!node->IsAlive()) {
        return;
    }

    LOG_DEBUG("Finishing chunk (Address: %v)",
        node->Descriptor.GetDefaultAddress());

    TDataNodeServiceProxy proxy(node->LightChannel);
    auto req = proxy.FinishChunk();
    req->SetTimeout(Config_->NodeRpcTimeout);
    ToProto(req->mutable_chunk_id(), ChunkId_);
    *req->mutable_chunk_meta() = ChunkMeta_;
    req->set_block_count(BlockCount_);

    auto rspOrError = WaitFor(req->Invoke());
    if (!rspOrError.IsOK()) {
        OnNodeFailed(node, rspOrError);
        return;
    }

    const auto& rsp = rspOrError.Value();
    const auto& chunkInfo = rsp->chunk_info();
    LOG_DEBUG("Chunk finished (Address: %v, DiskSpace: %v)",
        node->Descriptor.GetDefaultAddress(),
        chunkInfo.disk_space());

    ChunkInfo_ = chunkInfo;
}

void TReplicationWriter::SendPing(const TWeakPtr<TNode>& node_)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    auto node = node_.Lock();
    if (!node) {
        return;
    }

    LOG_DEBUG("Sending ping (Address: %v)",
        node->Descriptor.GetDefaultAddress());

    TDataNodeServiceProxy proxy(node->LightChannel);
    auto req = proxy.PingSession();
    req->SetTimeout(Config_->NodeRpcTimeout);
    ToProto(req->mutable_chunk_id(), ChunkId_);
    req->Invoke();
}

void TReplicationWriter::CancelWriter(bool abort)
{
    // No thread affinity; may be called from dtor.

    for (auto node : Nodes_) {
        CancelNode(node, abort);
    }
}

void TReplicationWriter::CancelNode(TNodePtr node, bool abort)
{
    if (node->Canceled.test_and_set()) {
        return;
    }

    node->PingExecutor->Stop();

    if (abort) {
        TDataNodeServiceProxy proxy(node->LightChannel);
        auto req = proxy.CancelChunk();
        ToProto(req->mutable_chunk_id(), ChunkId_);
        req->Invoke();
    }
}

bool TReplicationWriter::WriteBlock(const TSharedRef& block)
{
    return WriteBlocks(std::vector<TSharedRef>(1, block));
}

bool TReplicationWriter::WriteBlocks(const std::vector<TSharedRef>& blocks)
{
    YCHECK(IsOpen_);
    YCHECK(!IsClosing_);
    YCHECK(!State_.IsClosed());

    if (!State_.IsActive()) {
        return false;
    }

    WindowSlots_->Acquire(GetByteSize(blocks));
    TDispatcher::Get()->GetWriterInvoker()->Invoke(
        BIND(&TReplicationWriter::AddBlocks, MakeWeak(this), blocks));

    return WindowSlots_->IsReady();
}

TFuture<void> TReplicationWriter::GetReadyEvent()
{
    YCHECK(IsOpen_);
    YCHECK(!IsClosing_);
    YCHECK(!State_.HasRunningOperation());
    YCHECK(!State_.IsClosed());

    if (!WindowSlots_->IsReady()) {
        State_.StartOperation();

        // No need to capture #this by strong reference, because
        // WindowSlots are always released when Writer is alive,
        // and callback is called synchronously.
        WindowSlots_->GetReadyEvent().Subscribe(BIND([=] (const TError& error) {
            if (error.IsOK()) {
                State_.FinishOperation(TError());
            }
        }));
    }

    return State_.GetOperationError();
}

void TReplicationWriter::AddBlocks(const std::vector<TSharedRef>& blocks)
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YCHECK(!IsCloseRequested_);

    if (!State_.IsActive()) {
        return;
    }

    int firstBlockIndex = BlockCount_;
    int currentBlockIndex = firstBlockIndex;

    for (const auto& block : blocks) {
        EnsureCurrentGroup();

        auto blockId = TBlockId(ChunkId_, currentBlockIndex);
        BlockCache_->Put(blockId, EBlockType::CompressedData, block, Null);

        CurrentGroup_->AddBlock(block);

        ++BlockCount_;
        ++currentBlockIndex;

        if (CurrentGroup_->GetSize() >= Config_->GroupSize) {
            FlushCurrentGroup();
        }
    }

    int lastBlockIndex = BlockCount_ - 1;

    LOG_DEBUG("Blocks added (Blocks: %v-%v, Size: %v)",
        firstBlockIndex,
        lastBlockIndex,
        GetByteSize(blocks));
}

void TReplicationWriter::DoClose()
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YCHECK(!IsCloseRequested_);

    LOG_DEBUG("Writer close requested");

    if (!State_.IsActive()) {
        State_.FinishOperation();
        return;
    }

    if (CurrentGroup_ && CurrentGroup_->GetSize() > 0) {
        FlushCurrentGroup();
    }

    IsCloseRequested_ = true;

    if (Window_.empty()) {
        CloseSessions();
    }
}

TFuture<void> TReplicationWriter::Close(const TChunkMeta& chunkMeta)
{
    YCHECK(IsOpen_);
    YCHECK(!IsClosing_);
    YCHECK(!State_.HasRunningOperation());
    YCHECK(!State_.IsClosed());

    IsClosing_ = true;
    ChunkMeta_ = chunkMeta;

    LOG_DEBUG("Requesting writer to close");

    State_.StartOperation();

    TDispatcher::Get()->GetWriterInvoker()->Invoke(
        BIND(&TReplicationWriter::DoClose, MakeWeak(this)));

    return State_.GetOperationError();
}

const TChunkInfo& TReplicationWriter::GetChunkInfo() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return ChunkInfo_;
}

const TDataStatistics& TReplicationWriter::GetDataStatistics() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    YUNREACHABLE();
}

TChunkReplicaList TReplicationWriter::GetWrittenChunkReplicas() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TChunkReplicaList chunkReplicas;
    for (auto node : Nodes_) {
        if (node->IsAlive()) {
            chunkReplicas.push_back(node->ChunkReplica);
        }
    }
    return chunkReplicas;
}

TChunkId TReplicationWriter::GetChunkId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return ChunkId_;
}

NErasure::ECodec TReplicationWriter::GetErasureCodecId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NErasure::ECodec::None;
}

IChannelPtr TReplicationWriter::CreateRetryingNodeChannel(
    IChannelFactoryPtr channelFactory,
    const Stroka& address)
{
    return CreateRetryingChannel(
        Config_->NodeChannel,
        channelFactory->CreateChannel(address),
        BIND([] (const TError& error) {
            return error.FindMatching(NChunkClient::EErrorCode::WriteThrottlingActive).HasValue();
        }));
}

///////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateReplicationWriter(
    TReplicationWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TChunkId& chunkId,
    const TChunkReplicaList& targets,
    TNodeDirectoryPtr nodeDirectory,
    INativeClientPtr client,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
{
    return New<TReplicationWriter>(
        config,
        options,
        chunkId,
        targets,
        nodeDirectory,
        client,
        throttler,
        blockCache);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

