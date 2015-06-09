#include "stdafx.h"
#include "replication_writer.h"

#include "chunk_meta_extensions.h"
#include "chunk_replica.h"
#include "chunk_service_proxy.h"
#include "config.h"
#include "data_node_service_proxy.h"
#include "dispatcher.h"
#include "chunk_writer.h"
#include "block_cache.h"
#include "private.h"

#include <ytlib/node_tracker_client/node_directory.h>

#include <core/concurrency/async_semaphore.h>
#include <core/concurrency/scheduler.h>
#include <core/concurrency/periodic_executor.h>
#include <core/concurrency/thread_affinity.h>

#include <core/misc/address.h>
#include <core/misc/async_stream_state.h>
#include <core/misc/nullable.h>

#include <core/logging/log.h>

#include <deque>
#include <atomic>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NRpc;

///////////////////////////////////////////////////////////////////////////////

class TReplicationWriter;
typedef TIntrusivePtr<TReplicationWriter> TReplicationWriterPtr;

struct TNode
    : public TRefCounted
{
    const int Index;
    const TNodeDescriptor Descriptor;
    const TChunkReplica ChunkReplica;

    TError Error;
    TDataNodeServiceProxy LightProxy;
    TDataNodeServiceProxy HeavyProxy;
    TPeriodicExecutorPtr PingExecutor;
    std::atomic_flag Canceled;

    TNode(
        int index,
        const TNodeDescriptor& descriptor,
        TChunkReplica chunkReplica,
        IChannelPtr lightChannel,
        IChannelPtr heavyChannel,
        TDuration rpcTimeout)
        : Index(index)
        , Descriptor(descriptor)
        , ChunkReplica(chunkReplica)
        , LightProxy(lightChannel)
        , HeavyProxy(heavyChannel)
    {
        LightProxy.SetDefaultTimeout(rpcTimeout);
        HeavyProxy.SetDefaultTimeout(rpcTimeout);
        Canceled.clear();
    }

    bool IsAlive() const
    {
        return Error.IsOK();
    }

};

typedef TIntrusivePtr<TNode> TNodePtr;
typedef TWeakPtr<TNode> TNodeWeakPtr;

Stroka ToString(TNodePtr node)
{
    return node->Descriptor.GetDefaultAddress();
}

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

typedef TIntrusivePtr<TGroup> TGroupPtr;
typedef std::deque<TGroupPtr> TWindow;

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
        IChannelPtr masterChannel,
        IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache);

    ~TReplicationWriter();

    virtual TFuture<void> Open() override;

    virtual bool WriteBlock(const TSharedRef& block) override;
    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual TFuture<void> Close(const TChunkMeta& chunkMeta) override;

    virtual const TChunkInfo& GetChunkInfo() const override;
    virtual TChunkReplicaList GetWrittenChunkReplicas() const override;

    virtual TChunkId GetChunkId() const override;

private:
    friend class TGroup;

    const TReplicationWriterConfigPtr Config_;
    const TRemoteWriterOptionsPtr Options_;
    const TChunkId ChunkId_;
    const TChunkReplicaList InitialTargets_;
    const IChannelPtr MasterChannel_;
    const TNodeDirectoryPtr NodeDirectory_;
    const IThroughputThrottlerPtr Throttler_;
    const IBlockCachePtr BlockCache_;

    TAsyncStreamState State_;

    bool IsOpen_ = false;
    bool IsClosing_ = false;

    //! This flag is raised whenever #Close is invoked.
    //! All access to this flag happens from #WriterThread.
    bool IsCloseRequested_ = false;
    TChunkMeta ChunkMeta_;

    TWindow Window_;
    TAsyncSemaphore WindowSlots_;

    std::vector<TNodePtr> Nodes_;

    //! Number of nodes that are still alive.
    int AliveNodeCount_ = 0;

    const int MinUploadReplicationFactor_;

    //! A new group of blocks that is currently being filled in by the client.
    //! All access to this field happens from client thread.
    TGroupPtr CurrentGroup_;

    //! Number of blocks that are already added via #AddBlocks.
    int BlockCount_ = 0;

    //! Returned from node on Finish.
    TChunkInfo ChunkInfo_;

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

    void SendPing(TNodeWeakPtr node);

    void CancelWriter(bool abort);
    void CancelNode(TNodePtr node, bool abort);

    void AddBlocks(const std::vector<TSharedRef>& blocks);

    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);
};

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

    auto req = node->HeavyProxy.PutBlocks();
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

    for (int dstNodeIndex = 0; dstNodeIndex < SentTo_.size(); ++dstNodeIndex) {
        auto dstNode = writer->Nodes_[dstNodeIndex];
        if (dstNode->IsAlive() && !SentTo_[dstNodeIndex]) {
            LOG_DEBUG("Sending blocks (Blocks: %v-%v, SrcAddress: %v, DstAddress: %v)",
                GetStartBlockIndex(),
                GetEndBlockIndex(),
                srcNode->Descriptor.GetDefaultAddress(),
                dstNode->Descriptor.GetDefaultAddress());

            auto req = srcNode->LightProxy.SendBlocks();

            // Set double timeout for SendBlocks since executing it implies another (src->dst) RPC call.
            req->SetTimeout(writer->Config_->NodeRpcTimeout + writer->Config_->NodeRpcTimeout);
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
                writer->OnNodeFailed(
                    rspOrError.GetCode() == EErrorCode::PipelineFailed ? dstNode : srcNode,
                    rspOrError);
            }

            break;
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
    if (!writer || !writer->State_.IsActive())
        return;

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
    IChannelPtr masterChannel,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
    : Config_(config)
    , Options_(options)
    , ChunkId_(chunkId)
    , InitialTargets_(initialTargets)
    , MasterChannel_(masterChannel)
    , NodeDirectory_(nodeDirectory)
    , Throttler_(throttler)
    , BlockCache_(blockCache)
    , WindowSlots_(config->SendWindowSize)
    , MinUploadReplicationFactor_(std::min(Config_->UploadReplicationFactor, Config_->MinUploadReplicationFactor))
{
    Logger.AddTag("ChunkId: %v, ChunkWriter: %v", ChunkId_, this);
}

TReplicationWriter::~TReplicationWriter()
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Just a quick check.
    if (State_.IsClosed())
        return;

    LOG_INFO("Writer canceled");
    State_.Cancel(TError("Writer canceled"));

    CancelWriter(true);
}

TChunkReplicaList TReplicationWriter::AllocateTargets()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!MasterChannel_) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::MasterCommunicationFailed, 
            "Cannnot allocate more targets, no master channel provided");
    }

    TChunkServiceProxy proxy(MasterChannel_);

    auto req = proxy.AllocateWriteTargets();
    int activeTargets = Nodes_.size();
    req->set_desired_target_count(Config_->UploadReplicationFactor - activeTargets);
    req->set_min_target_count(std::max(Config_->MinUploadReplicationFactor - activeTargets, 1));
    req->set_replication_factor_override(Config_->UploadReplicationFactor);
    if (Config_->PreferLocalHost) {
        req->set_preferred_host_name(TAddressResolver::Get()->GetLocalHostName());
    }
    for (auto node : Nodes_) {
        req->add_forbidden_addresses(node->Descriptor.GetDefaultAddress());
    }
    ToProto(req->mutable_chunk_id(), ChunkId_);

    auto rspOrError = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        EErrorCode::MasterCommunicationFailed, 
        "Failed to allocate targets for chunk %v", 
        ChunkId_);
    const auto& rsp = rspOrError.Value();

    NodeDirectory_->MergeFrom(rsp->node_directory());

    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(rsp->replicas());
    return replicas;
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
    auto address = nodeDescriptor.GetAddressOrThrow(Options_->NetworkName);
    LOG_DEBUG("Starting write session (Address: %v)", address);

    TDataNodeServiceProxy proxy(LightNodeChannelFactory->CreateChannel(nodeDescriptor.GetInterconnectAddress()));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    auto req = proxy.StartChunk();
    ToProto(req->mutable_chunk_id(), ChunkId_);
    req->set_session_type(static_cast<int>(Options_->SessionType));
    req->set_sync_on_close(Config_->SyncOnClose);

    auto rspOrError = WaitFor(req->Invoke());
    if (!rspOrError.IsOK()) {
        LOG_WARNING(rspOrError, "Failed to start write session on node %v", address);
        return;
    }

    LOG_DEBUG("Write session started (Address: %v)", address);

    auto lightChannel = LightNodeChannelFactory->CreateChannel(address);
    auto heavyChannel = HeavyNodeChannelFactory->CreateChannel(address);
    auto node = New<TNode>(
        Nodes_.size(),
        nodeDescriptor,
        target,
        lightChannel,
        heavyChannel,
        Config_->NodeRpcTimeout);

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

        while (Nodes_.size() < Config_->UploadReplicationFactor) {
            StartSessions(AllocateTargets());
        }

        LOG_INFO("Writer opened (Addresses: [%v], PopulateCache: %v, SessionType: %v, Network: %v)",
            JoinToString(Nodes_),
            Config_->PopulateCache,
            Options_->SessionType,
            Options_->NetworkName);

        IsOpen_ = true;
    } catch (const std::exception& ex) {
        CancelWriter(true);
        THROW_ERROR_EXCEPTION("Not enough target nodes to start writing session for chunk %v",
            ChunkId_)
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

    if (lastFlushableBlock < 0)
        return;

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
        if (group->GetEndBlockIndex() > lastFlushedBlock)
            return;

        LOG_DEBUG("Window shifted (Blocks: %v-%v, Size: %v)",
            group->GetStartBlockIndex(),
            group->GetEndBlockIndex(),
            group->GetSize());

        WindowSlots_.Release(group->GetSize());
        Window_.pop_front();
    }

    if (State_.IsActive() && IsCloseRequested_) {
        CloseSessions();
    }
}

void TReplicationWriter::FlushBlocks(TNodePtr node, int blockIndex)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!node->IsAlive())
        return;

    LOG_DEBUG("Flushing block (Block: %v, Address: %v)",
        blockIndex,
        node->Descriptor.GetDefaultAddress());

    auto req = node->LightProxy.FlushBlocks();
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

    if (!State_.IsActive())
        return;

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

    if (!node->IsAlive())
        return;

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

    if (!node->IsAlive())
        return;

    LOG_DEBUG("Finishing chunk (Address: %v)",
        node->Descriptor.GetDefaultAddress());

    auto req = node->LightProxy.FinishChunk();
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

void TReplicationWriter::SendPing(TNodeWeakPtr node)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    auto node_ = node.Lock();
    if (!node_) {
        return;
    }

    LOG_DEBUG("Sending ping (Address: %v)",
        node_->Descriptor.GetDefaultAddress());

    auto req = node_->LightProxy.PingSession();
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
    if (node->Canceled.test_and_set())
        return;

    node->PingExecutor->Stop();

    if (abort) {
        auto req = node->LightProxy.CancelChunk();
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

    WindowSlots_.Acquire(GetByteSize(blocks));
    TDispatcher::Get()->GetWriterInvoker()->Invoke(
        BIND(&TReplicationWriter::AddBlocks, MakeWeak(this), blocks));

    return WindowSlots_.IsReady();
}

TFuture<void> TReplicationWriter::GetReadyEvent()
{
    YCHECK(IsOpen_);
    YCHECK(!IsClosing_);
    YCHECK(!State_.HasRunningOperation());
    YCHECK(!State_.IsClosed());

    if (!WindowSlots_.IsReady()) {
        State_.StartOperation();

        // No need to capture #this by strong reference, because
        // WindowSlots are always released when Writer is alive,
        // and callback is called synchronously.
        WindowSlots_.GetReadyEvent().Subscribe(BIND([=] (const TError& error) {
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

    if (!State_.IsActive())
        return;

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

///////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateReplicationWriter(
    TReplicationWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TChunkId& chunkId,
    const TChunkReplicaList& targets,
    TNodeDirectoryPtr nodeDirectory,
    IChannelPtr masterChannel,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
{
    return New<TReplicationWriter>(
        config,
        options,
        chunkId,
        targets,
        nodeDirectory,
        masterChannel,
        throttler,
        blockCache);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

