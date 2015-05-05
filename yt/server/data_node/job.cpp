#include "stdafx.h"
#include "job.h"
#include "chunk.h"
#include "chunk_store.h"
#include "block_store.h"
#include "location.h"
#include "config.h"
#include "journal_chunk.h"
#include "journal_dispatcher.h"
#include "session_manager.h"
#include "master_connector.h"
#include "session.h"
#include "private.h"

#include <core/misc/protobuf_helpers.h>
#include <core/misc/string.h>

#include <core/erasure/codec.h>

#include <core/concurrency/scheduler.h>

#include <core/actions/cancelable_context.h>

#include <core/logging/log.h>

#include <ytlib/node_tracker_client/helpers.h>
#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/chunk_client/chunk_writer.h>
#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/erasure_reader.h>
#include <ytlib/chunk_client/job.pb.h>
#include <ytlib/chunk_client/replication_writer.h>
#include <ytlib/chunk_client/replication_reader.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/api/client.h>

#include <server/hydra/changelog.h>

#include <server/job_agent/job.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

namespace NYT {
namespace NDataNode {

using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NJobTrackerClient;
using namespace NJobAgent;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCellNode;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;

using NNodeTrackerClient::TNodeDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const i64 ReadPriority = 0;

////////////////////////////////////////////////////////////////////////////////

class TChunkJobBase
    : public IJob
{
public:
    DEFINE_SIGNAL(void(const TNodeResources& resourcesDelta), ResourcesUpdated);

public:
    TChunkJobBase(
        const TJobId& jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : JobId_(jobId)
        , JobSpec_(jobSpec)
        , ResourceLimits_(resourceLimits)
        , Config_(config)
        , Bootstrap_(bootstrap)
    {
        Logger.AddTag("JobId: %v", jobId);
    }

    virtual void Start() override
    {
        JobState_ = EJobState::Running;
        JobPhase_ = EJobPhase::Running;

        try {
            DoPrepare();
        } catch (const std::exception& ex) {
            SetFailed(ex);
            return;
        }

        JobFuture_ = BIND(&TChunkJobBase::GuardedRun, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetControlInvoker())
            .Run();
    }

    virtual void Abort(const TError& error) override
    {
        switch (JobState_) {
            case EJobState::Waiting:
                SetAborted(error);
                return;

            case EJobState::Running:
                JobFuture_.Cancel();
                SetAborted(error);
                return;

            default:
                return;
        }
    }

    virtual const TJobId& GetId() const override
    {
        return JobId_;
    }

    virtual const TJobSpec& GetSpec() const override
    {
        return JobSpec_;
    }

    virtual EJobState GetState() const override
    {
        return JobState_;
    }

    virtual EJobPhase GetPhase() const override
    {
        return JobPhase_;
    }

    virtual TNodeResources GetResourceUsage() const override
    {
        return ResourceLimits_;
    }

    virtual void SetResourceUsage(const TNodeResources& /*newUsage*/) override
    {
        YUNREACHABLE();
    }

    virtual TJobResult GetResult() const override
    {
        return Result_;
    }

    virtual void SetResult(const TJobResult& /*result*/) override
    {
        YUNREACHABLE();
    }

    virtual double GetProgress() const override
    {
        return Progress_;
    }

    virtual void SetProgress(double value) override
    {
        Progress_ = value;
    }

    virtual void SetStatistics(const NYTree::TYsonString& /*statistics*/) override
    {
        YUNREACHABLE();
    }

    virtual std::vector<TChunkId> DumpInputContexts() const override
    {
        THROW_ERROR_EXCEPTION("Input context dumping is not supported");
    }

    virtual NYTree::TYsonString Strace() const override
    {
        THROW_ERROR_EXCEPTION("Stracing is not supported");
    }

protected:
    const TJobId JobId_;
    const TJobSpec JobSpec_;
    TNodeResources ResourceLimits_;
    const TDataNodeConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    NLogging::TLogger Logger = DataNodeLogger;

    EJobState JobState_ = EJobState::Waiting;
    EJobPhase JobPhase_ = EJobPhase::Created;

    double Progress_ = 0.0;

    TFuture<void> JobFuture_;

    TJobResult Result_;

    TChunkId ChunkId_;


    virtual void DoPrepare()
    {
        const auto& chunkSpecExt = JobSpec_.GetExtension(TChunkJobSpecExt::chunk_job_spec_ext);
        ChunkId_ = FromProto<TChunkId>(chunkSpecExt.chunk_id());

        Logger.AddTag("ChunkId: %v", ChunkId_);
    }

    virtual void DoRun() = 0;


    void GuardedRun()
    {
        LOG_INFO("Job started (JobType: %v)",
            EJobType(JobSpec_.type()));
        try {
            DoRun();
        } catch (const std::exception& ex) {
            SetFailed(ex);
            return;
        }
        SetCompleted();
    }

    void SetCompleted()
    {
        LOG_INFO("Job completed");
        Progress_ = 1.0;
        DoSetFinished(EJobState::Completed, TError());
    }

    void SetFailed(const TError& error)
    {
        LOG_ERROR(error, "Job failed");
        DoSetFinished(EJobState::Failed, error);
    }

    void SetAborted(const TError& error)
    {
        LOG_INFO(error, "Job aborted");
        DoSetFinished(EJobState::Aborted, error);
    }

private:
    void DoSetFinished(EJobState finalState, const TError& error)
    {
        if (JobState_ != EJobState::Running)
            return;

        JobPhase_ = EJobPhase::Finished;
        JobState_ = finalState;
        ToProto(Result_.mutable_error(), error);
        auto deltaResources = ZeroNodeResources() - ResourceLimits_;
        ResourceLimits_ = ZeroNodeResources();
        JobFuture_.Reset();
        ResourcesUpdated_.Fire(deltaResources);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TLocalChunkJobBase
    : public TChunkJobBase
{
public:
    TLocalChunkJobBase(
        const TJobId& jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TChunkJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
    { }

protected:
    IChunkPtr Chunk_;
    TChunkReadGuard ChunkReadGuard_;


    virtual void DoPrepare()
    {
        TChunkJobBase::DoPrepare();

        auto chunkStore = Bootstrap_->GetChunkStore();
        Chunk_ = chunkStore->GetChunkOrThrow(ChunkId_);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkRemovalJob
    : public TLocalChunkJobBase
{
public:
    TChunkRemovalJob(
        const TJobId& jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TLocalChunkJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
    { }

private:
    virtual void DoRun() override
    {
        auto sessionManager = Bootstrap_->GetSessionManager();
        auto session = sessionManager->FindSession(Chunk_->GetId());
        if (session) {
            session->Cancel(TError("Chunk is removed"));
        }

        auto chunkStore = Bootstrap_->GetChunkStore();
        WaitFor(chunkStore->RemoveChunk(Chunk_))
            .ThrowOnError();
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkReplicationJob
    : public TLocalChunkJobBase
{
public:
    TChunkReplicationJob(
        const TJobId& jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TLocalChunkJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , ReplicateChunkJobSpecExt_(JobSpec_.GetExtension(TReplicateChunkJobSpecExt::replicate_chunk_job_spec_ext))
    { }

private:
    const TReplicateChunkJobSpecExt ReplicateChunkJobSpecExt_;


    virtual void DoRun() override
    {
        auto metaOrError = WaitFor(Chunk_->ReadMeta(0));
        THROW_ERROR_EXCEPTION_IF_FAILED(
            metaOrError,
            "Error getting meta of chunk %v",
            ChunkId_);

        LOG_INFO("Chunk meta fetched");
        const auto& meta = metaOrError.Value();

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(ReplicateChunkJobSpecExt_.node_directory());

        auto targets = FromProto<TChunkReplica, TChunkReplicaList>(ReplicateChunkJobSpecExt_.targets());

        auto options = New<TRemoteWriterOptions>();
        options->SessionType = EWriteSessionType::Replication;

        auto writer = CreateReplicationWriter(
            Config_->ReplicationWriter,
            options,
            ChunkId_,
            targets,
            nodeDirectory,
            nullptr,
            Bootstrap_->GetReplicationOutThrottler());

        {
            auto error = WaitFor(writer->Open());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                error,
                "Error opening writer for chunk %v during replication",
                ChunkId_);
        }

        int blockIndex = 0;
        int blockCount = GetBlockCount(*meta);

        auto blockStore = Bootstrap_->GetBlockStore();

        while (blockIndex < blockCount) {
            auto getResult = WaitFor(blockStore->ReadBlocks(
                ChunkId_,
                blockIndex,
                blockCount - blockIndex,
                ReadPriority,
                false));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                getResult,
                "Error reading chunk %v during replication",
                ChunkId_);
            const auto& blocks = getResult.Value();

            LOG_DEBUG("Enqueuing blocks for replication (Blocks: %v-%v)",
                blockIndex,
                blockIndex + blocks.size() - 1);

            auto writeResult = writer->WriteBlocks(blocks);
            if (!writeResult) {
                auto error = WaitFor(writer->GetReadyEvent());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    error,
                    "Error writing chunk %v during replication",
                    ChunkId_);
            }

            blockIndex += blocks.size();
        }

        LOG_DEBUG("All blocks are enqueued for replication");

        {
            auto error = WaitFor(writer->Close(*meta));
            THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error closing replication writer");
        }
    }

    int GetBlockCount(const TChunkMeta& meta)
    {
        switch (TypeFromId(DecodeChunkId(ChunkId_).Id)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk: {
                auto blocksExt = GetProtoExtension<TBlocksExt>(meta.extensions());
                return blocksExt.blocks_size();
            }

            case EObjectType::JournalChunk: {
                auto miscExt = GetProtoExtension<TMiscExt>(meta.extensions());
                if (!miscExt.sealed()) {
                    THROW_ERROR_EXCEPTION("Cannot replicate an unsealed chunk %v",
                        ChunkId_);
                }
                return miscExt.row_count();
            }

            default:
                YUNREACHABLE();
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkRepairJob
    : public TChunkJobBase
{
public:
    TChunkRepairJob(
        const TJobId& jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TChunkJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , RepairJobSpecExt_(JobSpec_.GetExtension(TRepairChunkJobSpecExt::repair_chunk_job_spec_ext))
    { }

private:
    const TRepairChunkJobSpecExt RepairJobSpecExt_;


    virtual void DoRun() override
    {
        auto codecId = NErasure::ECodec(RepairJobSpecExt_.erasure_codec());
        auto* codec = NErasure::GetCodec(codecId);

        auto replicas = FromProto<TChunkReplica>(RepairJobSpecExt_.replicas());
        auto targets = FromProto<TChunkReplica>(RepairJobSpecExt_.targets());
        auto erasedIndexes = FromProto<int, NErasure::TPartIndexList>(RepairJobSpecExt_.erased_indexes());
        YCHECK(targets.size() == erasedIndexes.size());

        // Compute repair plan.
        auto repairIndexes = codec->GetRepairIndices(erasedIndexes);
        if (!repairIndexes) {
            THROW_ERROR_EXCEPTION("Codec is unable to repair the chunk");
        }

        LOG_INFO("Preparing to repair (ErasedIndexes: [%v], RepairIndexes: [%v], Targets: [%v])",
            JoinToString(erasedIndexes),
            JoinToString(*repairIndexes),
            JoinToString(targets));

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(RepairJobSpecExt_.node_directory());

        std::vector<IChunkReaderPtr> readers;
        for (int partIndex : *repairIndexes) {
            TChunkReplicaList partReplicas;
            for (auto replica : replicas) {
                if (replica.GetIndex() == partIndex) {
                    partReplicas.push_back(replica);
                }
            }
            YCHECK(!partReplicas.empty());

            auto partId = ErasurePartIdFromChunkId(ChunkId_, partIndex);
            auto options = New<TRemoteReaderOptions>();
            options->SessionType = EReadSessionType::Repair;
            auto reader = CreateReplicationReader(
                Config_->RepairReader,
                options,
                Bootstrap_->GetBlockCache(),
                Bootstrap_->GetMasterClient()->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower),
                nodeDirectory,
                Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
                partId,
                partReplicas,
                Bootstrap_->GetRepairInThrottler());
            readers.push_back(reader);
        }

        std::vector<IChunkWriterPtr> writers;
        for (int index = 0; index < static_cast<int>(erasedIndexes.size()); ++index) {
            int partIndex = erasedIndexes[index];
            auto partId = ErasurePartIdFromChunkId(ChunkId_, partIndex);
            auto options = New<TRemoteWriterOptions>();
            options->SessionType = EWriteSessionType::Repair;
            auto writer = CreateReplicationWriter(
                Config_->RepairWriter,
                options,
                partId,
                TChunkReplicaList(1, targets[index]),
                nodeDirectory,
                nullptr,
                Bootstrap_->GetRepairOutThrottler());
            writers.push_back(writer);
        }

        {
            auto onProgress = BIND(&TChunkRepairJob::SetProgress, MakeWeak(this))
                .Via(GetCurrentInvoker());

            auto result = RepairErasedParts(
                codec,
                erasedIndexes,
                readers,
                writers,
                onProgress);

            auto repairError = WaitFor(result);
            THROW_ERROR_EXCEPTION_IF_FAILED(repairError, "Error repairing chunk %v",
                ChunkId_);
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TSealChunkJob
    : public TLocalChunkJobBase
{
public:
    TSealChunkJob(
        const TJobId& jobId,
        TJobSpec&& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TLocalChunkJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , SealJobSpecExt_(JobSpec_.GetExtension(TSealChunkJobSpecExt::seal_chunk_job_spec_ext))
    { }

private:
    const TSealChunkJobSpecExt SealJobSpecExt_;


    virtual void DoRun() override
    {
        if (Chunk_->GetType() != EObjectType::JournalChunk) {
            THROW_ERROR_EXCEPTION("Cannot seal a non-journal chunk %v",
                ChunkId_);
        }

        auto journalChunk = Chunk_->AsJournalChunk();
        if (journalChunk->IsActive()) {
            THROW_ERROR_EXCEPTION("Cannot seal an active journal chunk %v",
                ChunkId_);
        }

        auto readGuard = TChunkReadGuard::TryAcquire(Chunk_);
        if (!readGuard) {
            THROW_ERROR_EXCEPTION("Cannot lock chunk %v",
                ChunkId_);
        }

        auto journalDispatcher = Bootstrap_->GetJournalDispatcher();
        auto location = journalChunk->GetLocation();
        auto changelog = WaitFor(journalDispatcher->OpenChangelog(location, ChunkId_))
            .ValueOrThrow();

        if (journalChunk->HasAttachedChangelog()) {
            THROW_ERROR_EXCEPTION("Journal chunk %v is already being written to",
                ChunkId_);
        }

        TJournalChunkChangelogGuard changelogGuard(journalChunk, changelog);

        if (changelog->IsSealed()) {
            LOG_INFO("Chunk %v is already sealed",
                ChunkId_);
            return;
        }

        i64 currentRowCount = changelog->GetRecordCount();
        i64 sealRowCount = SealJobSpecExt_.row_count();
        if (currentRowCount < sealRowCount) {
            LOG_INFO("Started downloading missing journal chunk rows (Rows: %v-%v)",
                currentRowCount,
                sealRowCount - 1);

            auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
            nodeDirectory->MergeFrom(SealJobSpecExt_.node_directory());

            auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(SealJobSpecExt_.replicas());

            auto options = New<TRemoteReaderOptions>();
            options->SessionType = EReadSessionType::Replication;
            auto reader = CreateReplicationReader(
                Config_->SealReader,
                options,
                Bootstrap_->GetBlockCache(),
                Bootstrap_->GetMasterClient()->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower),
                nodeDirectory,
                Null,
                ChunkId_,
                replicas,
                Bootstrap_->GetReplicationInThrottler());

            while (currentRowCount < sealRowCount) {
                auto blocksOrError = WaitFor(reader->ReadBlocks(
                    currentRowCount,
                    sealRowCount - currentRowCount));
                THROW_ERROR_EXCEPTION_IF_FAILED(blocksOrError);

                const auto& blocks = blocksOrError.Value();
                int blockCount = static_cast<int>(blocks.size());

                if (blockCount == 0) {
                    THROW_ERROR_EXCEPTION("Cannot download missing rows %v-%v to seal chunk %v",
                        currentRowCount,
                        sealRowCount - 1,
                        ChunkId_);
                }

                LOG_INFO("Journal chunk rows downloaded (Rows: %v-%v)",
                    currentRowCount,
                    currentRowCount + blockCount - 1);

                for (const auto& block : blocks) {
                    changelog->Append(block);
                }

                currentRowCount += blockCount;
            }

            LOG_INFO("Finished downloading missing journal chunk rows");
        }

        LOG_INFO("Started sealing journal chunk (RowCount: %v)",
            sealRowCount);
        {
            auto error = WaitFor(changelog->Seal(sealRowCount));
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }
        LOG_INFO("Finished sealing journal chunk");

        auto chunkStore = Bootstrap_->GetChunkStore();
        chunkStore->UpdateExistingChunk(Chunk_);
    }

};

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateChunkJob(
    const TJobId& jobId,
    TJobSpec&& jobSpec,
    const TNodeResources& resourceLimits,
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap)
{
    auto type = EJobType(jobSpec.type());
    switch (type) {
        case EJobType::ReplicateChunk:
            return New<TChunkReplicationJob>(
                jobId,
                std::move(jobSpec),
                resourceLimits,
                config,
                bootstrap);

        case EJobType::RemoveChunk:
            return New<TChunkRemovalJob>(
                jobId,
                std::move(jobSpec),
                resourceLimits,
                config,
                bootstrap);

        case EJobType::RepairChunk:
            return New<TChunkRepairJob>(
                jobId,
                std::move(jobSpec),
                resourceLimits,
                config,
                bootstrap);

        case EJobType::SealChunk:
            return New<TSealChunkJob>(
                jobId,
                std::move(jobSpec),
                resourceLimits,
                config,
                bootstrap);

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

