#include "job.h"

#include "bootstrap.h"
#include "private.h"
#include "block_peer_table.h"
#include "chunk_block_manager.h"
#include "chunk.h"
#include "chunk_store.h"
#include "config.h"
#include "journal_chunk.h"
#include "journal_dispatcher.h"
#include "location.h"
#include "master_connector.h"

#include <yt/yt/server/lib/chunk_server/proto/job.pb.h>

#include <yt/yt/server/lib/hydra/changelog.h>

#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/master_connector.h>

#include <yt/yt/server/node/job_agent/job.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/chunk_client/block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/yt/ytlib/chunk_client/confirming_writer.h>
#include <yt/yt/ytlib/chunk_client/meta_aggregating_writer.h>
#include <yt/yt/ytlib/chunk_client/deferred_chunk_meta.h>
#include <yt/yt/ytlib/chunk_client/erasure_repair.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>
#include <yt/yt/ytlib/chunk_client/replication_reader.h>
#include <yt/yt/ytlib/chunk_client/replication_writer.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>

#include <yt/yt/ytlib/job_tracker_client/proto/job.pb.h>

#include <yt/yt/ytlib/journal_client/erasure_repair.h>
#include <yt/yt/ytlib/journal_client/chunk_reader.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>

#include <yt/yt/ytlib/table_client/chunk_state.h>
#include <yt/yt/ytlib/table_client/columnar_chunk_meta.h>
#include <yt/yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/yt/ytlib/table_client/schemaless_multi_chunk_reader.h>
#include <yt/yt/ytlib/table_client/schemaless_chunk_writer.h>

#include <yt/yt/library/erasure/impl/codec.h>

#include <yt/yt/core/actions/cancelable_context.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/protobuf_helpers.h>
#include <yt/yt/core/misc/string.h>

#include <yt/yt/client/api/client.h>

#include <yt/yt/client/chunk_client/read_limit.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_batch.h>
#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/client/transaction_client/public.h>

#include <util/generic/algorithm.h>

namespace NYT::NDataNode {

using namespace NApi;
using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NJobTrackerClient;
using namespace NJobAgent;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NClusterNode;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NYson;
using namespace NCoreDump;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NIO;
using namespace NTracing;

using NNodeTrackerClient::TNodeDescriptor;
using NChunkClient::TChunkReaderStatistics;
using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TMasterJobBase
    : public IJob
{
public:
    DEFINE_SIGNAL_OVERRIDE(void(const TNodeResources& resourcesDelta), ResourcesUpdated);
    DEFINE_SIGNAL_OVERRIDE(void(), PortsReleased);
    DEFINE_SIGNAL_OVERRIDE(void(), JobPrepared);
    DEFINE_SIGNAL_OVERRIDE(void(), JobFinished);

public:
    TMasterJobBase(
        TJobId jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : JobId_(jobId)
        , JobSpec_(jobSpec)
        , Config_(config)
        , StartTime_(TInstant::Now())
        , Bootstrap_(bootstrap)
        , Logger(DataNodeLogger.WithTag("JobId: %v, JobType: %v",
            JobId_,
            GetType()))
        , ResourceLimits_(resourceLimits)
    {
        VERIFY_THREAD_AFFINITY(JobThread);
    }

    void Start() override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        JobState_ = EJobState::Running;
        JobPhase_ = EJobPhase::Running;
        JobFuture_ = BIND(&TMasterJobBase::GuardedRun, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetJobInvoker())
            .Run();
    }

    void Abort(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        switch (JobState_) {
            case EJobState::Waiting:
                SetAborted(error);
                return;

            case EJobState::Running:
                JobFuture_.Cancel(error);
                SetAborted(error);
                return;

            default:
                return;
        }
    }

    void Fail() override
    {
        THROW_ERROR_EXCEPTION("Failing is not supported");
    }

    TJobId GetId() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return JobId_;
    }

    TOperationId GetOperationId() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return {};
    }

    EJobType GetType() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return CheckedEnumCast<EJobType>(JobSpec_.type());
    }

    const TJobSpec& GetSpec() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return JobSpec_;
    }

    int GetPortCount() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return 0;
    }

    EJobState GetState() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return JobState_;
    }

    EJobPhase GetPhase() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return JobPhase_;
    }

    int GetSlotIndex() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return -1;
    }

    TNodeResources GetResourceUsage() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return ResourceLimits_;
    }

    std::vector<int> GetPorts() const override
    {
        YT_ABORT();
    }

    void SetPorts(const std::vector<int>&) override
    {
        YT_ABORT();
    }

    void SetResourceUsage(const TNodeResources& /*newUsage*/) override
    {
        YT_ABORT();
    }

    TJobResult GetResult() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return Result_;
    }

    void SetResult(const TJobResult& /*result*/) override
    {
        YT_ABORT();
    }

    double GetProgress() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return Progress_;
    }

    void SetProgress(double value) override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        Progress_ = value;
    }

    i64 GetStderrSize() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return JobStderrSize_;
    }

    void SetStderrSize(i64 value) override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        JobStderrSize_ = value;
    }

    void SetStderr(const TString& /*value*/) override
    {
        YT_ABORT();
    }

    void SetFailContext(const TString& /*value*/) override
    {
        YT_ABORT();
    }

    void SetProfile(const TJobProfile& /*value*/) override
    {
        YT_ABORT();
    }

    void SetCoreInfos(TCoreInfos /*value*/) override
    {
        YT_ABORT();
    }

    const TChunkCacheStatistics& GetChunkCacheStatistics() const override
    {
        const static TChunkCacheStatistics EmptyChunkCacheStatistics;
        return EmptyChunkCacheStatistics;
    }

    TYsonString GetStatistics() const override
    {
        return TYsonString();
    }

    void SetStatistics(const TYsonString& /*statistics*/) override
    {
        YT_ABORT();
    }

    void BuildOrchid(NYTree::TFluentMap /*fluent*/) const override
    { }

    TInstant GetStartTime() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return StartTime_;
    }

    NJobAgent::TTimeStatistics GetTimeStatistics() const override
    {
        return NJobAgent::TTimeStatistics{};
    }

    TInstant GetStatisticsLastSendTime() const override
    {
        YT_ABORT();
    }

    void ResetStatisticsLastSendTime() override
    {
        YT_ABORT();
    }

    std::vector<TChunkId> DumpInputContext() override
    {
        THROW_ERROR_EXCEPTION("Input context dumping is not supported");
    }

    std::optional<TString> GetStderr() override
    {
        THROW_ERROR_EXCEPTION("Getting stderr is not supported");
    }

    std::optional<TString> GetFailContext() override
    {
        THROW_ERROR_EXCEPTION("Getting fail context is not supported");
    }

    TPollJobShellResponse PollJobShell(
        const NJobProberClient::TJobShellDescriptor& /*jobShellDescriptor*/,
        const TYsonString& /*parameters*/) override
    {
        THROW_ERROR_EXCEPTION("Job shell is not supported");
    }

    void Interrupt() override
    {
        THROW_ERROR_EXCEPTION("Interrupting is not supported");
    }

    void OnJobProxySpawned() override
    {
        YT_ABORT();
    }

    void PrepareArtifact(
        const TString& /*artifactName*/,
        const TString& /*pipePath*/) override
    {
        YT_ABORT();
    }

    void OnArtifactPreparationFailed(
        const TString& /*artifactName*/,
        const TString& /*artifactPath*/,
        const TError& /*error*/) override
    {
        YT_ABORT();
    }

    void OnArtifactsPrepared() override
    {
        YT_ABORT();
    }

    void OnJobPrepared() override
    {
        YT_ABORT();
    }

    void HandleJobReport(TNodeJobReport&&) override
    {
        YT_ABORT();
    }

    void ReportSpec() override
    {
        YT_ABORT();
    }

    void ReportStderr() override
    {
        YT_ABORT();
    }

    void ReportFailContext() override
    {
        YT_ABORT();
    }

    void ReportProfile() override
    {
        YT_ABORT();
    }

    bool GetStored() const override
    {
        return false;
    }

    void SetStored(bool /* value */) override
    {
        YT_ABORT();
    }

protected:
    const TJobId JobId_;
    const TJobSpec JobSpec_;
    const TDataNodeConfigPtr Config_;
    const TInstant StartTime_;
    IBootstrap* const Bootstrap_;

    const NLogging::TLogger Logger;

    TNodeResources ResourceLimits_;

    EJobState JobState_ = EJobState::Waiting;
    EJobPhase JobPhase_ = EJobPhase::Created;

    double Progress_ = 0.0;
    ui64 JobStderrSize_ = 0;

    TString Stderr_;

    TFuture<void> JobFuture_;

    TJobResult Result_;

    DECLARE_THREAD_AFFINITY_SLOT(JobThread);

    virtual void DoRun() = 0;

    void GuardedRun()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto context = TTraceContext::NewRoot(Format("%vJob.Run", GetType()));
        TCurrentTraceContextGuard guard(context);
        auto baggage = context->UnpackOrCreateBaggage();
        baggage->Set("job_id", ToString(GetId()));
        baggage->Set("job_type@", ToString(GetType()));
        context->PackBaggage(std::move(baggage));

        try {
            JobPrepared_.Fire();
            WaitFor(BIND(&TMasterJobBase::DoRun, MakeStrong(this))
                .AsyncVia(Bootstrap_->GetMasterJobInvoker())
                .Run())
                .ThrowOnError();
        } catch (const std::exception& ex) {
            SetFailed(ex);
            return;
        }
        SetCompleted();
    }

    void SetCompleted()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_INFO("Job completed");
        Progress_ = 1.0;
        DoSetFinished(EJobState::Completed, TError());
    }

    void SetFailed(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_ERROR(error, "Job failed");
        DoSetFinished(EJobState::Failed, error);
    }

    void SetAborted(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_INFO(error, "Job aborted");
        DoSetFinished(EJobState::Aborted, error);
    }

    IChunkPtr FindLocalChunk(TChunkId chunkId, int mediumIndex)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        return chunkStore->FindChunk(chunkId, mediumIndex);
    }

    IChunkPtr GetLocalChunkOrThrow(TChunkId chunkId, int mediumIndex)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        return chunkStore->GetChunkOrThrow(chunkId, mediumIndex);
    }

private:
    void DoSetFinished(EJobState finalState, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (JobState_ != EJobState::Running && JobState_ != EJobState::Waiting) {
            return;
        }

        JobPhase_ = EJobPhase::Finished;
        JobState_ = finalState;
        JobFinished_.Fire();
        ToProto(Result_.mutable_error(), error);
        auto deltaResources = ZeroNodeResources() - ResourceLimits_;
        ResourceLimits_ = ZeroNodeResources();
        JobFuture_.Reset();
        ResourcesUpdated_.Fire(deltaResources);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkRemovalJob
    : public TMasterJobBase
{
public:
    TChunkRemovalJob(
        TJobId jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : TMasterJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , JobSpecExt_(JobSpec_.GetExtension(TRemoveChunkJobSpecExt::remove_chunk_job_spec_ext))
    { }

private:
    const TRemoveChunkJobSpecExt JobSpecExt_;

    void DoRun() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto chunkId = FromProto<TChunkId>(JobSpecExt_.chunk_id());
        int mediumIndex = JobSpecExt_.medium_index();
        auto replicas = FromProto<TChunkReplicaList>(JobSpecExt_.replicas());
        auto replicasExpirationDeadline = FromProto<TInstant>(JobSpecExt_.replicas_expiration_deadline());
        auto chunkIsDead = JobSpecExt_.chunk_is_dead();

        YT_LOG_INFO("Chunk removal job started (ChunkId: %v@%v, Replicas: %v, ReplicasExpirationDeadline: %v, ChunkIsDead: %v)",
            chunkId,
            mediumIndex,
            replicas,
            replicasExpirationDeadline,
            chunkIsDead);

        // TODO(ifsmirnov, akozhikhov): Consider DRT here.

        auto chunk = chunkIsDead
            ? FindLocalChunk(chunkId, mediumIndex)
            : GetLocalChunkOrThrow(chunkId, mediumIndex);
        if (!chunk) {
            YT_VERIFY(chunkIsDead);
            YT_LOG_INFO("Dead chunk is missing, reporting success");
            return;
        }

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        WaitFor(chunkStore->RemoveChunk(chunk))
            .ThrowOnError();

        // Wait for the removal notification to be delivered to master.
        // Cf. YT-6532.
        // Once we switch from push replication to pull, this code is likely
        // to appear in TReplicateChunkJob as well.
        YT_LOG_INFO("Waiting for heartbeat barrier");
        const auto& masterConnector = Bootstrap_->GetMasterConnector();
        WaitFor(masterConnector->GetHeartbeatBarrier(CellTagFromId(chunkId)))
            .ThrowOnError();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkReplicationJob
    : public TMasterJobBase
{
public:
    TChunkReplicationJob(
        TJobId jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : TMasterJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , JobSpecExt_(JobSpec_.GetExtension(TReplicateChunkJobSpecExt::replicate_chunk_job_spec_ext))
    { }

private:
    const TReplicateChunkJobSpecExt JobSpecExt_;

    void DoRun() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto chunkId = FromProto<TChunkId>(JobSpecExt_.chunk_id());
        int sourceMediumIndex = JobSpecExt_.source_medium_index();
        auto targetReplicas = FromProto<TChunkReplicaWithMediumList>(JobSpecExt_.target_replicas());

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(JobSpecExt_.node_directory());

        // Compute target medium index.
        if (targetReplicas.empty()) {
            THROW_ERROR_EXCEPTION("No target replicas");
        }
        int targetMediumIndex = targetReplicas[0].GetMediumIndex();
        auto sessionId = TSessionId(chunkId, targetMediumIndex);

        YT_LOG_INFO("Chunk replication job started (ChunkId: %v@%v, TargetReplicas: %v)",
            chunkId,
            sourceMediumIndex,
            MakeFormattableView(targetReplicas, TChunkReplicaAddressFormatter(nodeDirectory)));

        TWorkloadDescriptor workloadDescriptor;
        workloadDescriptor.Category = EWorkloadCategory::SystemReplication;
        workloadDescriptor.Annotations.push_back(Format("Replication of chunk %v",
            chunkId));

        auto chunk = GetLocalChunkOrThrow(chunkId, sourceMediumIndex);

        TChunkReadOptions chunkReadOptions;
        chunkReadOptions.WorkloadDescriptor = workloadDescriptor;
        chunkReadOptions.BlockCache = Bootstrap_->GetBlockCache();
        chunkReadOptions.ChunkReaderStatistics = New<TChunkReaderStatistics>();

        TRefCountedChunkMetaPtr meta;
        {
            YT_LOG_DEBUG("Fetching chunk meta");

            auto asyncMeta = chunk->ReadMeta(chunkReadOptions);
            meta = WaitFor(asyncMeta)
                .ValueOrThrow();

            YT_LOG_DEBUG("Chunk meta fetched");
        }

        auto options = New<TRemoteWriterOptions>();
        options->AllowAllocatingNewTargetNodes = false;

        auto writer = CreateReplicationWriter(
            Config_->ReplicationWriter,
            options,
            sessionId,
            std::move(targetReplicas),
            nodeDirectory,
            Bootstrap_->GetMasterClient(),
            GetNullBlockCache(),
            /* trafficMeter */ nullptr,
            Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::ReplicationOut));

        {
            YT_LOG_DEBUG("Started opening writer");

            WaitFor(writer->Open())
                .ThrowOnError();

            YT_LOG_DEBUG("Writer opened");
        }

        int currentBlockIndex = 0;
        int blockCount = GetBlockCount(chunkId, *meta);
        while (currentBlockIndex < blockCount) {

            const auto& chunkBlockManager = Bootstrap_->GetChunkBlockManager();
            auto asyncReadBlocks = chunkBlockManager->ReadBlockRange(
                chunkId,
                currentBlockIndex,
                blockCount - currentBlockIndex,
                chunkReadOptions);

            auto readBlocks = WaitFor(asyncReadBlocks)
                .ValueOrThrow();

            i64 totalBlockSize = 0;
            for (const auto& block : readBlocks) {
                if (block) {
                    totalBlockSize += block.Size();
                }
            }
            if (totalBlockSize > 0 && Bootstrap_->GetIOTracker()->IsEnabled()) {
                Bootstrap_->GetIOTracker()->Enqueue(TIOCounters{
                    .ByteCount = totalBlockSize,
                    .IOCount = 1
                },
                /*tags*/ {});
            }

            std::vector<TBlock> writeBlocks;
            for (const auto& block : readBlocks) {
                if (!block)
                    break;
                writeBlocks.push_back(block);
            }

            YT_LOG_DEBUG("Enqueuing blocks for replication (Blocks: %v-%v)",
                currentBlockIndex,
                currentBlockIndex + static_cast<int>(writeBlocks.size()) - 1);

            auto writeResult = writer->WriteBlocks(writeBlocks);
            if (!writeResult) {
                WaitFor(writer->GetReadyEvent())
                    .ThrowOnError();
            }

            currentBlockIndex += writeBlocks.size();
        }

        YT_LOG_DEBUG("All blocks are enqueued for replication");

        {
            YT_LOG_DEBUG("Started closing writer");

            auto deferredMeta = New<TDeferredChunkMeta>();
            deferredMeta->MergeFrom(*meta);

            WaitFor(writer->Close(deferredMeta))
                .ThrowOnError();

            YT_LOG_DEBUG("Writer closed");
        }
    }

    static int GetBlockCount(TChunkId chunkId, const TChunkMeta& meta)
    {
        switch (TypeFromId(DecodeChunkId(chunkId).Id)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk: {
                auto blocksExt = GetProtoExtension<TBlocksExt>(meta.extensions());
                return blocksExt.blocks_size();
            }

            case EObjectType::JournalChunk:
            case EObjectType::ErasureJournalChunk: {
                auto miscExt = GetProtoExtension<TMiscExt>(meta.extensions());
                if (!miscExt.sealed()) {
                    THROW_ERROR_EXCEPTION("Cannot replicate an unsealed chunk %v",
                        chunkId);
                }
                return miscExt.row_count();
            }

            default:
                YT_ABORT();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkRepairJob
    : public TMasterJobBase
{
public:
    TChunkRepairJob(
        TJobId jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : TMasterJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , JobSpecExt_(JobSpec_.GetExtension(TRepairChunkJobSpecExt::repair_chunk_job_spec_ext))
        , ChunkId_(FixChunkId(FromProto<TChunkId>(JobSpecExt_.chunk_id())))
        , SourceReplicas_(FromProto<TChunkReplicaList>(JobSpecExt_.source_replicas()))
        , TargetReplicas_(FromProto<TChunkReplicaWithMediumList>(JobSpecExt_.target_replicas()))
    { }

private:
    const TRepairChunkJobSpecExt JobSpecExt_;
    const TChunkId ChunkId_;
    const TChunkReplicaList SourceReplicas_;
    const TChunkReplicaWithMediumList TargetReplicas_;

    const NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();


    // COMPAT(babenko): pre-20.2 master servers may send encoded chunk id, which is inappropriate.
    static TChunkId FixChunkId(TChunkId chunkId)
    {
        auto type = TypeFromId(chunkId);
        if (type >= MinErasureChunkPartType && type <= MaxErasureChunkPartType) {
            return ReplaceTypeInId(chunkId, EObjectType::ErasureChunk);
        }
        return chunkId;
    }

    IChunkReaderPtr CreateReader(int partIndex)
    {
        TChunkReplicaList partReplicas;
        for (auto replica : SourceReplicas_) {
            if (replica.GetReplicaIndex() == partIndex) {
                partReplicas.push_back(replica);
            }
        }

        if (partReplicas.empty()) {
            THROW_ERROR_EXCEPTION("No source replicas for part %v",
                partIndex);
        }

        auto options = New<TRemoteReaderOptions>();
        options->AllowFetchingSeedsFromMaster = false;

        auto partChunkId = ErasurePartIdFromChunkId(ChunkId_, partIndex);
        auto reader = CreateReplicationReader(
            Config_->RepairReader,
            options,
            Bootstrap_->GetMasterClient(),
            NodeDirectory_,
            Bootstrap_->GetLocalDescriptor(),
            Bootstrap_->GetNodeId(),
            partChunkId,
            partReplicas,
            Bootstrap_->GetBlockCache(),
            /*chunkMetaCache*/ nullptr,
            /*trafficMeter*/ nullptr,
            /*nodeStatusDirectory*/ nullptr,
            Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::RepairIn),
            /*rpsThrottler*/ GetUnlimitedThrottler());

        return reader;
    }

    IChunkWriterPtr CreateWriter(int partIndex)
    {
        auto targetReplica = [&] {
            for (auto replica : TargetReplicas_) {
                if (replica.GetReplicaIndex() == partIndex) {
                    return replica;
                }
            }
            YT_ABORT();
        }();
        auto partChunkId = ErasurePartIdFromChunkId(ChunkId_, partIndex);
        auto partSessionId = TSessionId(partChunkId, targetReplica.GetMediumIndex());
        auto options = New<TRemoteWriterOptions>();
        options->AllowAllocatingNewTargetNodes = false;
        auto writer = CreateReplicationWriter(
            Config_->RepairWriter,
            options,
            partSessionId,
            TChunkReplicaWithMediumList(1, targetReplica),
            NodeDirectory_,
            Bootstrap_->GetMasterClient(),
            GetNullBlockCache(),
            /* trafficMeter */ nullptr,
            Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::RepairOut));
        return writer;
    }

    void DoRun() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto codecId = CheckedEnumCast<NErasure::ECodec>(JobSpecExt_.erasure_codec());
        auto* codec = NErasure::GetCodec(codecId);
        auto decommission = JobSpecExt_.decommission();
        auto rowCount = JobSpecExt_.has_row_count() ? std::make_optional<i64>(JobSpecExt_.row_count()) : std::nullopt;

        NodeDirectory_->MergeFrom(JobSpecExt_.node_directory());

        YT_LOG_INFO("Chunk repair job started (ChunkId: %v, Codec: %v, "
            "SourceReplicas: %v, TargetReplicas: %v, Decommission: %v, RowCount: %v)",
            ChunkId_,
            codecId,
            MakeFormattableView(SourceReplicas_, TChunkReplicaAddressFormatter(NodeDirectory_)),
            MakeFormattableView(TargetReplicas_, TChunkReplicaAddressFormatter(NodeDirectory_)),
            decommission,
            rowCount);

        TWorkloadDescriptor workloadDescriptor;
        workloadDescriptor.Category = decommission ? EWorkloadCategory::SystemReplication : EWorkloadCategory::SystemRepair;
        workloadDescriptor.Annotations.push_back(Format("%v of chunk %v",
            decommission ? "Decommission via repair" : "Repair",
            ChunkId_));

        // TODO(savrus): profile chunk reader statistics.
        TClientChunkReadOptions chunkReadOptions{
            .WorkloadDescriptor = workloadDescriptor
        };

        NErasure::TPartIndexList sourcePartIndexes;
        for (auto replica : SourceReplicas_) {
            sourcePartIndexes.push_back(replica.GetReplicaIndex());
        }
        SortUnique(sourcePartIndexes);

        NErasure::TPartIndexList erasedPartIndexes;
        for (auto replica : TargetReplicas_) {
            erasedPartIndexes.push_back(replica.GetReplicaIndex());
        }
        SortUnique(erasedPartIndexes);

        std::vector<IChunkWriterPtr> writers;
        for (int partIndex : erasedPartIndexes) {
            writers.push_back(CreateWriter(partIndex));
        }

        {
            TFuture<void> future;
            auto chunkType = TypeFromId(ChunkId_);
            switch (chunkType) {
                case EObjectType::ErasureChunk: {
                    auto repairPartIndexes = codec->GetRepairIndices(erasedPartIndexes);
                    if (!repairPartIndexes) {
                        THROW_ERROR_EXCEPTION("Codec is unable to repair the chunk");
                    }

                    std::vector<IChunkReaderPtr> readers;
                    for (int partIndex : *repairPartIndexes) {
                        readers.push_back(CreateReader(partIndex));
                    }

                    future = NChunkClient::RepairErasedParts(
                        codec,
                        erasedPartIndexes,
                        readers,
                        writers,
                        chunkReadOptions);
                    break;
                }

                case EObjectType::ErasureJournalChunk: {
                    std::vector<IChunkReaderPtr> readers;
                    for (int partIndex : sourcePartIndexes) {
                        readers.push_back(CreateReader(partIndex));
                    }

                    future = NJournalClient::RepairErasedParts(
                        Config_->RepairReader,
                        codec,
                        *rowCount,
                        erasedPartIndexes,
                        readers,
                        writers,
                        chunkReadOptions,
                        Logger);
                    break;
                }

                default:
                    THROW_ERROR_EXCEPTION("Unsupported chunk type %Qlv",
                        chunkType);
            }

            WaitFor(future)
                .ThrowOnError();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSealChunkJob
    : public TMasterJobBase
{
public:
    TSealChunkJob(
        TJobId jobId,
        TJobSpec&& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : TMasterJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            config,
            bootstrap)
        , JobSpecExt_(JobSpec_.GetExtension(TSealChunkJobSpecExt::seal_chunk_job_spec_ext))
    { }

private:
    const TSealChunkJobSpecExt JobSpecExt_;

    void DoRun() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto chunkId = FromProto<TChunkId>(JobSpecExt_.chunk_id());
        auto codecId = CheckedEnumCast<NErasure::ECodec>(JobSpecExt_.codec_id());
        int mediumIndex = JobSpecExt_.medium_index();
        auto sourceReplicas = FromProto<TChunkReplicaList>(JobSpecExt_.source_replicas());
        i64 sealRowCount = JobSpecExt_.row_count();

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(JobSpecExt_.node_directory());

        YT_LOG_INFO("Chunk seal job started (ChunkId: %v@%v, Codec: %v, SourceReplicas: %v, RowCount: %v)",
            chunkId,
            mediumIndex,
            codecId,
            MakeFormattableView(sourceReplicas, TChunkReplicaAddressFormatter(nodeDirectory)),
            sealRowCount);

        auto chunk = GetLocalChunkOrThrow(chunkId, mediumIndex);
        if (!chunk->IsJournalChunk()) {
            THROW_ERROR_EXCEPTION("Cannot seal a non-journal chunk %v",
                chunkId);
        }

        auto journalChunk = chunk->AsJournalChunk();
        if (journalChunk->IsSealed()) {
            YT_LOG_INFO("Chunk is already sealed");
            return;
        }

        TWorkloadDescriptor workloadDescriptor;
        workloadDescriptor.Category = EWorkloadCategory::SystemTabletLogging;
        workloadDescriptor.Annotations.push_back(Format("Seal of chunk %v",
            chunkId));

        auto updateGuard = TChunkUpdateGuard::Acquire(chunk);

        const auto& journalDispatcher = Bootstrap_->GetJournalDispatcher();
        const auto& location = journalChunk->GetStoreLocation();
        auto changelog = WaitFor(journalDispatcher->OpenChangelog(location, chunkId))
            .ValueOrThrow();

        i64 currentRowCount = changelog->GetRecordCount();
        if (currentRowCount < sealRowCount) {
            YT_LOG_DEBUG("Job will read missing journal chunk rows (Rows: %v-%v)",
                currentRowCount,
                sealRowCount - 1);

            auto reader = NJournalClient::CreateChunkReader(
                Config_->SealReader,
                Bootstrap_->GetMasterClient(),
                nodeDirectory,
                chunkId,
                codecId,
                sourceReplicas,
                Bootstrap_->GetBlockCache(),
                /*chunkMetaCache*/ nullptr,
                /*trafficMeter*/ nullptr,
                Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::ReplicationIn));

            // TODO(savrus): profile chunk reader statistics.
            TClientChunkReadOptions chunkReadOptions{
                .WorkloadDescriptor = workloadDescriptor
            };

            while (currentRowCount < sealRowCount) {
                YT_LOG_DEBUG("Reading rows (Rows: %v-%v)",
                    currentRowCount,
                    sealRowCount - 1);

                auto asyncBlocks = reader->ReadBlocks(
                    chunkReadOptions,
                    currentRowCount,
                    sealRowCount - currentRowCount);
                auto blocks = WaitFor(asyncBlocks)
                    .ValueOrThrow();

                int blockCount = blocks.size();
                if (blockCount == 0) {
                    THROW_ERROR_EXCEPTION("Rows %v-%v are missing but needed to seal chunk %v",
                        currentRowCount,
                        sealRowCount - 1,
                        chunkId);
                }

                YT_LOG_DEBUG("Rows received (Rows: %v-%v)",
                    currentRowCount,
                    currentRowCount + blockCount - 1);

                std::vector<TSharedRef> records;
                records.reserve(blocks.size());
                for (const auto& block : blocks) {
                    records.push_back(block.Data);
                }
                changelog->Append(records);

                i64 totalRecordsSize = 0;
                for (const auto& block : blocks) {
                    totalRecordsSize += block.Size();
                }
                if (totalRecordsSize > 0 && Bootstrap_->GetIOTracker()->IsEnabled()) {
                    Bootstrap_->GetIOTracker()->Enqueue(TIOCounters{
                        .ByteCount = totalRecordsSize,
                        .IOCount = 1
                    },
                    /*tags*/ {});
                }

                currentRowCount += blockCount;
            }

            WaitFor(changelog->Flush())
                .ThrowOnError();

            YT_LOG_DEBUG("Finished downloading missing journal chunk rows");
        }

        YT_LOG_DEBUG("Started sealing journal chunk (RowCount: %v)",
            sealRowCount);

        WaitFor(journalChunk->Seal())
            .ThrowOnError();

        YT_LOG_DEBUG("Finished sealing journal chunk");

        journalChunk->UpdateFlushedRowCount(changelog->GetRecordCount());
        journalChunk->UpdateDataSize(changelog->GetDataSize());

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        chunkStore->UpdateExistingChunk(chunk);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkMergeJob
    : public TMasterJobBase
{
public:
    TChunkMergeJob(
        TJobId jobId,
        const TJobSpec& jobSpec,
        const TNodeResources& resourceLimits,
        TDataNodeConfigPtr config,
        IBootstrap* bootstrap)
        : TMasterJobBase(
            jobId,
            std::move(jobSpec),
            resourceLimits,
            std::move(config),
            bootstrap)
        , JobSpecExt_(JobSpec_.GetExtension(TMergeChunksJobSpecExt::merge_chunks_job_spec_ext))
        , CellTag_(FromProto<TCellTag>(JobSpecExt_.cell_tag()))
    { }

private:
    const TMergeChunksJobSpecExt JobSpecExt_;
    const TCellTag CellTag_;


    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_;
    TTableSchemaPtr Schema_;
    NCompression::ECodec CompressionCodec_;
    NErasure::ECodec ErasureCodec_;
    std::optional<EOptimizeFor> OptimizeFor_;
    std::optional<bool> EnableSkynetSharing_;
    int MaxHeavyColumns_;
    std::optional<int> MaxBlockCount_;

    struct TChunkInfo
    {
        IChunkReaderPtr Reader;
        TDeferredChunkMetaPtr Meta;
        TChunkId ChunkId;
        int BlockCount;
        TClientChunkReadOptions Options;
    };
    std::vector<TChunkInfo> InputChunkInfos_;

    void DoRun() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        NodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
        NodeDirectory_->MergeFrom(JobSpecExt_.node_directory());

        const auto& chunkMergerWriterOptions = JobSpecExt_.chunk_merger_writer_options();
        Schema_ = New<TTableSchema>(FromProto<TTableSchema>(chunkMergerWriterOptions.schema()));
        CompressionCodec_ = CheckedEnumCast<NCompression::ECodec>(chunkMergerWriterOptions.compression_codec());
        ErasureCodec_ = CheckedEnumCast<NErasure::ECodec>(chunkMergerWriterOptions.erasure_codec());
        if (chunkMergerWriterOptions.has_optimize_for()) {
            OptimizeFor_ = CheckedEnumCast<EOptimizeFor>(chunkMergerWriterOptions.optimize_for());
        }
        if (chunkMergerWriterOptions.has_enable_skynet_sharing()) {
            EnableSkynetSharing_ = chunkMergerWriterOptions.enable_skynet_sharing();
        }
        MaxHeavyColumns_ = chunkMergerWriterOptions.max_heavy_columns();

        auto mergeMode = CheckedEnumCast<NChunkClient::EChunkMergerMode>(chunkMergerWriterOptions.merge_mode());
        YT_LOG_DEBUG("Merge job started (Mode: %v)", mergeMode);

        PrepareInputChunkMetas();
        switch (mergeMode) {
            case NChunkClient::EChunkMergerMode::Shallow:
                MergeShallow();
                break;
            case NChunkClient::EChunkMergerMode::Deep:
                MergeDeep();
                break;
            case NChunkClient::EChunkMergerMode::Auto:
                try {
                    MergeShallow();
                } catch (const TErrorException& ex) {
                    if (ex.Error().GetCode() != NChunkClient::EErrorCode::IncompatibleChunkMetas) {
                        throw;
                    }
                    YT_LOG_DEBUG(ex, "Unable to merge chunks using shallow mode, falling back to deep merge");
                    MergeDeep();
                }
                break;
            default:
                THROW_ERROR_EXCEPTION("Cannot merge chunks in %Qlv mode", mergeMode);
        }
    }

    void PrepareInputChunkMetas()
    {
        for (const auto& chunk : JobSpecExt_.input_chunks()) {
            auto reader = CreateReader(chunk);
            auto chunkId = FromProto<TChunkId>(chunk.id());

            TWorkloadDescriptor workloadDescriptor;
            workloadDescriptor.Category = EWorkloadCategory::SystemMerge;
            workloadDescriptor.Annotations.push_back(Format("Merge chunk %v", chunkId));

            TClientChunkReadOptions options;
            options.WorkloadDescriptor = workloadDescriptor;

            auto chunkMeta = GetChunkMeta(reader, options);
            auto blocksExt = GetProtoExtension<NTableClient::NProto::TBlockMetaExt>(chunkMeta->extensions());

            InputChunkInfos_.push_back({
                .Reader = std::move(reader),
                .Meta = std::move(chunkMeta),
                .ChunkId = chunkId,
                .BlockCount = blocksExt.blocks_size(),
                .Options = options
            });
        }
    }

    void MergeShallow()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto confirmingWriter = CreateWriter();

        auto options = New<TMetaAggregatingWriterOptions>();
        options->TableSchema = Schema_;
        options->CompressionCodec = CompressionCodec_;
        options->ErasureCodec = ErasureCodec_;
        if (EnableSkynetSharing_) {
            options->EnableSkynetSharing = *EnableSkynetSharing_;
        }
        options->MaxHeavyColumns = MaxHeavyColumns_;

        auto writer = CreateMetaAggregatingWriter(
            confirmingWriter,
            options);
        WaitFor(writer->Open())
            .ThrowOnError();

        int totalBlockCount = 0;
        for (const auto& chunkInfo : InputChunkInfos_) {
            writer->AbsorbMeta(chunkInfo.Meta, chunkInfo.ChunkId);
            totalBlockCount += chunkInfo.BlockCount;
        }

        if (MaxBlockCount_ && totalBlockCount > *MaxBlockCount_) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::IncompatibleChunkMetas,
                "Too many blocks for shallow merge")
                << TErrorAttribute("actual_total_block_count", totalBlockCount)
                << TErrorAttribute("max_allowed_total_block_count", *MaxBlockCount_);
        }

        for (const auto& chunkInfo : InputChunkInfos_) {
            int currentBlockCount = 0;
            auto inputChunkBlockCount = chunkInfo.BlockCount;
            while (currentBlockCount < inputChunkBlockCount) {
                auto asyncResult = chunkInfo.Reader->ReadBlocks(
                    chunkInfo.Options,
                    currentBlockCount,
                    inputChunkBlockCount - currentBlockCount);

                auto readResult = WaitFor(asyncResult);
                THROW_ERROR_EXCEPTION_IF_FAILED(readResult, "Error reading blocks");
                auto blocks = readResult.Value();
                if (!writer->WriteBlocks(blocks)) {
                    auto writeResult = WaitFor(writer->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(writeResult, "Error writing block");
                }
                currentBlockCount += ssize(blocks);
            }
        }

        WaitFor(writer->Close())
            .ThrowOnError();
    }

    void MergeDeep()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto confirmingWriter = CreateWriter();

        auto chunkWriterOptions = New<TChunkWriterOptions>();
        chunkWriterOptions->CompressionCodec = CompressionCodec_;
        if (OptimizeFor_) {
            chunkWriterOptions->OptimizeFor = *OptimizeFor_;
        }
        if (EnableSkynetSharing_) {
            chunkWriterOptions->EnableSkynetSharing = *EnableSkynetSharing_;
        }

        auto writer = CreateSchemalessChunkWriter(
            New<TChunkWriterConfig>(),
            chunkWriterOptions,
            Schema_,
            confirmingWriter);

        auto rowBuffer = New<TRowBuffer>();
        auto writeNameTable = writer->GetNameTable();

        for (int i = 0; i < ssize(InputChunkInfos_); ++i) {
            auto chunkState = New<TChunkState>(
                Bootstrap_->GetBlockCache(),
                GetChunkSpec(JobSpecExt_.input_chunks()[i]),
                nullptr,
                NullTimestamp,
                nullptr,
                nullptr,
                nullptr,
                nullptr);

            const auto& chunkInfo = InputChunkInfos_[i];

            auto reader = CreateSchemalessRangeChunkReader(
                std::move(chunkState),
                New<TColumnarChunkMeta>(*chunkInfo.Meta),
                TChunkReaderConfig::GetDefault(),
                TChunkReaderOptions::GetDefault(),
                chunkInfo.Reader,
                New<TNameTable>(),
                chunkInfo.Options,
                /*keyColumns*/ {},
                /*omittedInaccessibleColumns*/ {},
                NTableClient::TColumnFilter(),
                NChunkClient::TReadRange());

            while (auto batch = WaitForRowBatch(reader)) {
                auto rows = batch->MaterializeRows();

                const auto& readerNameTable = reader->GetNameTable();
                auto readerTableSize = readerNameTable->GetSize();
                TNameTableToSchemaIdMapping idMapping(readerTableSize);
                const auto& names = readerNameTable->GetNames();
                for (auto i = 0; i < readerTableSize; ++i) {
                    idMapping[i] = writeNameTable->GetIdOrRegisterName(names[i]);
                }

                std::vector<TUnversionedRow> permutedRows;
                permutedRows.reserve(rows.size());
                for (auto row : rows) {
                    auto permutedRow = rowBuffer->CaptureAndPermuteRow(
                        row,
                        *Schema_,
                        Schema_->GetColumnCount(),
                        idMapping,
                        nullptr);
                    permutedRows.push_back(permutedRow);
                }

                writer->Write(MakeRange(permutedRows));
            }
        }

        WaitFor(writer->Close())
            .ThrowOnError();
    }

    IChunkWriterPtr CreateWriter()
    {
        auto outputChunkId = FromProto<TChunkId>(JobSpecExt_.output_chunk_id());
        int mediumIndex = JobSpecExt_.medium_index();
        auto sessionId = TSessionId(outputChunkId, mediumIndex);
        auto targetReplicas = FromProto<TChunkReplicaWithMediumList>(JobSpecExt_.target_replicas());

        auto options = New<TMultiChunkWriterOptions>();
        options->TableSchema = Schema_;
        options->CompressionCodec = CompressionCodec_;
        options->ErasureCodec = ErasureCodec_;

        return CreateConfirmingWriter(
            Config_->MergeWriter,
            options,
            CellTag_,
            NullTransactionId,
            NullChunkListId,
            NodeDirectory_,
            Bootstrap_->GetMasterClient(),
            Bootstrap_->GetBlockCache(),
            /*trafficMeter*/ nullptr,
            Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::MergeOut),
            sessionId,
            std::move(targetReplicas));
    }

    TChunkSpec GetChunkSpec(const NChunkClient::NProto::TMergeChunkInfo& chunk)
    {
        TChunkSpec chunkSpec;
        chunkSpec.set_row_count_override(chunk.row_count());
        chunkSpec.set_erasure_codec(chunk.erasure_codec()),
        *chunkSpec.mutable_chunk_id() = chunk.id();
        chunkSpec.mutable_replicas()->CopyFrom(chunk.source_replicas());

        return chunkSpec;
    }

    IChunkReaderPtr CreateReader(const NChunkClient::NProto::TMergeChunkInfo& chunk)
    {
        auto inputChunkId = FromProto<TChunkId>(chunk.id());
        YT_LOG_INFO("Reading input chunk (ChunkId: %v)", inputChunkId);

        auto erasureReaderConfig = New<TErasureReaderConfig>();
        erasureReaderConfig->EnableAutoRepair = false;

        return CreateRemoteReader(
            GetChunkSpec(chunk),
            erasureReaderConfig,
            New<TRemoteReaderOptions>(),
            Bootstrap_->GetMasterClient(),
            NodeDirectory_,
            Bootstrap_->GetLocalDescriptor(),
            Bootstrap_->GetNodeId(),
            Bootstrap_->GetBlockCache(),
            /*chunkMetaCache*/ nullptr,
            /*trafficMeter*/ nullptr,
            /*nodeStatusDirectory*/ nullptr,
            Bootstrap_->GetThrottler(NDataNode::EDataNodeThrottlerKind::MergeIn),
            /*rpsThrottler*/ GetUnlimitedThrottler());
    }

    TDeferredChunkMetaPtr GetChunkMeta(IChunkReaderPtr reader, const TClientChunkReadOptions& options)
    {
        auto result = WaitFor(reader->GetMeta(options));
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Merge job failed");

        auto deferredChunkMeta = New<TDeferredChunkMeta>();
        deferredChunkMeta->CopyFrom(*result.Value());
        return deferredChunkMeta;
    }
};

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateMasterJob(
    TJobId jobId,
    TJobSpec&& jobSpec,
    const TNodeResources& resourceLimits,
    TDataNodeConfigPtr config,
    IBootstrap* bootstrap)
{
    auto type = CheckedEnumCast<EJobType>(jobSpec.type());
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

        case EJobType::MergeChunks:
            return New<TChunkMergeJob>(
                jobId,
                std::move(jobSpec),
                resourceLimits,
                config,
                bootstrap);

        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

