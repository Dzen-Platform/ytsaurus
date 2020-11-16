#include "mutation_committer.h"
#include "private.h"
#include "changelog.h"
#include "config.h"
#include "decorated_automaton.h"
#include "checkpointer.h"
#include "mutation_context.h"
#include "serialize.h"

#include <yt/ytlib/election/cell_manager.h>
#include <yt/ytlib/election/config.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/timing.h>

#include <yt/core/tracing/trace_context.h>

#include <yt/core/rpc/response_keeper.h>

#include <utility>

namespace NYT::NHydra {

using namespace NElection;
using namespace NYTree;
using namespace NConcurrency;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto AutoSnapshotCheckPeriod = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

TCommitterBase::TCommitterBase(
    TDistributedHydraManagerConfigPtr config,
    const TDistributedHydraManagerOptions& options,
    TDecoratedAutomatonPtr decoratedAutomaton,
    TEpochContext* epochContext,
    NLogging::TLogger logger,
    NProfiling::TProfiler profiler)
    : Config_(std::move(config))
    , Options_(options)
    , DecoratedAutomaton_(std::move(decoratedAutomaton))
    , EpochContext_(epochContext)
    , Logger(std::move(logger))
    , Profiler(std::move(profiler))
    , CellManager_(EpochContext_->CellManager)
{
    YT_VERIFY(Config_);
    YT_VERIFY(DecoratedAutomaton_);
    YT_VERIFY(EpochContext_);
    VERIFY_INVOKER_THREAD_AFFINITY(EpochContext_->EpochControlInvoker, ControlThread);
    VERIFY_INVOKER_THREAD_AFFINITY(EpochContext_->EpochUserAutomatonInvoker, AutomatonThread);
}

void TCommitterBase::SuspendLogging()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YT_VERIFY(!LoggingSuspended_);

    YT_LOG_DEBUG("Mutations logging suspended");

    LoggingSuspended_ = true;
    LoggingSuspensionTimer_.emplace();
    LoggingSuspensionTimeoutCookie_ = TDelayedExecutor::Submit(
        BIND(&TCommitterBase::OnLoggingSuspensionTimeout, MakeWeak(this))
            .Via(EpochContext_->EpochUserAutomatonInvoker),
        Config_->MutationLoggingSuspensionTimeout);

    DoSuspendLogging();
}

void TCommitterBase::ResumeLogging()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YT_VERIFY(LoggingSuspended_);

    YT_LOG_DEBUG("Mutations logging resumed");

    Profiler.Update(LoggingSuspensionTimeGauge_, NProfiling::DurationToValue(LoggingSuspensionTimer_->GetElapsedTime()));

    LoggingSuspended_ = false;
    LoggingSuspensionTimer_.reset();
    TDelayedExecutor::CancelAndClear(LoggingSuspensionTimeoutCookie_);

    DoResumeLogging();
}

bool TCommitterBase::IsLoggingSuspended() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return LoggingSuspended_;
}

void TCommitterBase::OnLoggingSuspensionTimeout()
{
    LoggingFailed_.Fire(TError("Mutation logging is suspended for too long")
        << TErrorAttribute("timeout", Config_->MutationLoggingSuspensionTimeout));
}

////////////////////////////////////////////////////////////////////////////////

class TLeaderCommitter::TBatch
    : public TRefCounted
{
public:
    TBatch(
        TLeaderCommitter* owner,
        TVersion startVersion)
        : Owner_(owner)
        , StartVersion_(startVersion)
        , Logger(owner->Logger)
    { }

    void AddMutation(
        const TDecoratedAutomaton::TPendingMutation& pendingMutation,
        TSharedRef recordData,
        TFuture<void> localFlushFuture)
    {
        YT_VERIFY(GetStartVersion().Advance(GetMutationCount()) == pendingMutation.Version);

        BatchedRecordsData_.push_back(std::move(recordData));
        LocalFlushFuture_ = std::move(localFlushFuture);

        YT_LOG_DEBUG("Mutation batched (Version: %v, StartVersion: %v, SequenceNumber: %v, RandomSeed: %llx, PrevRandomSeed: %llx, MutationType: %v, MutationId: %v, TraceId: %v)",
            pendingMutation.Version,
            GetStartVersion(),
            pendingMutation.SequenceNumber,
            pendingMutation.RandomSeed,
            pendingMutation.PrevRandomSeed,
            pendingMutation.Request.Type,
            pendingMutation.Request.MutationId,
            pendingMutation.TraceContext ? pendingMutation.TraceContext->GetTraceId() : NTracing::TTraceId());
    }

    TFuture<TVersion> GetQuorumFlushFuture() const
    {
        return QuorumFlushPromise_;
    }

    void Flush(TVersion committedVersion)
    {
        auto owner = Owner_.Lock();
        if (!owner) {
            return;
        }

        YT_LOG_DEBUG("Flushing batched mutations (StartVersion: %v, MutationCount: %v)",
            GetStartVersion(),
            GetMutationCount());

        owner->Profiler.Enqueue("/commit_batch_size", GetMutationCount(), EMetricType::Gauge);

        std::vector<TFuture<void>> futures;

        CommitTimer_.emplace();

        if (!BatchedRecordsData_.empty()) {
            YT_VERIFY(LocalFlushFuture_);
            futures.push_back(LocalFlushFuture_.Apply(
                BIND(&TBatch::OnLocalFlush, MakeStrong(this))
                    .AsyncVia(owner->EpochContext_->EpochControlInvoker)));

            for (auto followerId = 0; followerId < owner->CellManager_->GetTotalPeerCount(); ++followerId) {
                if (followerId == owner->CellManager_->GetSelfPeerId()) {
                    continue;
                }

                auto channel = owner->CellManager_->GetPeerChannel(followerId);
                if (!channel) {
                    continue;
                }

                YT_LOG_DEBUG("Sending mutations to follower (PeerId: %v, StartVersion: %v, MutationCount: %v)",
                    followerId,
                    GetStartVersion(),
                    GetMutationCount());

                THydraServiceProxy proxy(channel);
                proxy.SetDefaultTimeout(owner->Config_->CommitFlushRpcTimeout);

                auto request = proxy.AcceptMutations();
                ToProto(request->mutable_epoch_id(), owner->EpochContext_->EpochId);
                request->set_start_revision(GetStartVersion().ToRevision());
                request->set_committed_revision(committedVersion.ToRevision());
                request->Attachments() = BatchedRecordsData_;

                futures.push_back(request->Invoke().Apply(
                    BIND(&TBatch::OnRemoteFlush, MakeStrong(this), followerId)
                        .AsyncVia(owner->EpochContext_->EpochControlInvoker)));
            }
        }

        AllSucceeded(std::move(futures)).Subscribe(
            BIND(&TBatch::OnCompleted, MakeStrong(this))
                .Via(owner->EpochContext_->EpochControlInvoker));
    }

    int GetMutationCount() const
    {
        return static_cast<int>(BatchedRecordsData_.size());
    }

    TVersion GetStartVersion() const
    {
        return StartVersion_;
    }

private:
    const TWeakPtr<TLeaderCommitter> Owner_;
    const TVersion StartVersion_;

    const NLogging::TLogger& Logger;

    // Counting with the local flush.
    int FlushCount_ = 0;

    TFuture<void> LocalFlushFuture_;
    const TPromise<TVersion> QuorumFlushPromise_ = NewPromise<TVersion>();
    std::vector<TSharedRef> BatchedRecordsData_;
    std::optional<TWallTimer> CommitTimer_;


    void OnRemoteFlush(TPeerId followerId, const THydraServiceProxy::TErrorOrRspAcceptMutationsPtr& rspOrError)
    {
        auto owner = Owner_.Lock();
        if (!owner) {
            return;
        }

        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Error logging mutations at follower (PeerId: %v, StartVersion: %v, MutationCount: %v)",
                followerId,
                GetStartVersion(),
                GetMutationCount());
            return;
        }

        const auto& rsp = rspOrError.Value();
        if (rsp->logged()) {
            auto voting = owner->CellManager_->GetPeerConfig(followerId).Voting;
            YT_LOG_DEBUG("Mutations are logged by follower (PeerId: %v, Voting: %v, StartVersion: %v, MutationCount: %v, WallTime: %v)",
                followerId,
                GetStartVersion(),
                GetMutationCount(),
                CommitTimer_->GetElapsedTime(),
                voting);
            if (voting) {
                OnSuccessfulFlush(owner);
            }
        } else {
            YT_LOG_DEBUG("Mutations are acknowledged by follower (PeerId: %v, StartVersion: %v, MutationCount: %v, WallTime: %v)",
                followerId,
                GetStartVersion(),
                GetMutationCount(),
                CommitTimer_->GetElapsedTime());
        }
    }

    void OnLocalFlush(const TError& error)
    {
        auto owner = Owner_.Lock();
        if (!owner) {
            return;
        }

        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        if (!error.IsOK()) {
            SetFailed(
                owner,
                TError(
                    NRpc::EErrorCode::Unavailable,
                    "Mutations are uncertain: local commit failed")
                    << error);
            return;
        }

        YT_LOG_DEBUG("Mutations are flushed locally (StartVersion: %v, MutationCount: %v, WallTime: %v)",
            GetStartVersion(),
            GetMutationCount(),
            CommitTimer_->GetElapsedTime());

        OnSuccessfulFlush(owner);
    }

    void OnCompleted(const TError&)
    {
        auto owner = Owner_.Lock();
        if (!owner) {
            return;
        }

        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        SetFailed(
            owner,
            TError(
                NRpc::EErrorCode::Unavailable,
                "Mutations are uncertain: %v out of %v commits were successful",
                FlushCount_,
                owner->CellManager_->GetTotalPeerCount()));
    }


    void OnSuccessfulFlush(const TLeaderCommitterPtr& owner)
    {
        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        ++FlushCount_;
        if (FlushCount_ == owner->CellManager_->GetQuorumPeerCount()) {
            SetSucceeded(owner);
        }
    }

    void SetSucceeded(const TLeaderCommitterPtr& owner)
    {
        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        if (QuorumFlushPromise_.IsSet()) {
            return;
        }

        owner->Profiler.Update(owner->CommitTimeGauge_, CommitTimer_->GetElapsedValue());

        YT_LOG_DEBUG("Mutations are flushed by quorum (StartVersion: %v, MutationCount: %v, WallTime: %v)",
            GetStartVersion(),
            GetMutationCount(),
            CommitTimer_->GetElapsedTime());

        auto committedVersion = GetStartVersion().Advance(GetMutationCount());
        QuorumFlushPromise_.Set(committedVersion);
    }

    void SetFailed(const TLeaderCommitterPtr& owner, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(owner->ControlThread);

        if (QuorumFlushPromise_.IsSet()) {
            return;
        }

        QuorumFlushPromise_.Set(error);

        owner->EpochContext_->EpochUserAutomatonInvoker->Invoke(BIND(
            &TLeaderCommitter::FireCommitFailed,
            owner,
            error));
    }
};

////////////////////////////////////////////////////////////////////////////////

TLeaderCommitter::TLeaderCommitter(
    TDistributedHydraManagerConfigPtr config,
    const TDistributedHydraManagerOptions& options,
    TDecoratedAutomatonPtr decoratedAutomaton,
    TEpochContext* epochContext,
    NLogging::TLogger logger,
    NProfiling::TProfiler profiler)
    : TCommitterBase(
        std::move(config),
        options,
        std::move(decoratedAutomaton),
        epochContext,
        std::move(logger),
        std::move(profiler))
    , AutoSnapshotCheckExecutor_(New<TPeriodicExecutor>(
        EpochContext_->EpochUserAutomatonInvoker,
        BIND(&TLeaderCommitter::OnAutoSnapshotCheck, MakeWeak(this)),
        AutoSnapshotCheckPeriod))
    , BatchAlarm_(New<TInvokerAlarm>(
        EpochContext_->EpochUserAutomatonInvoker))
{
    AutoSnapshotCheckExecutor_->Start();
}

TLeaderCommitter::~TLeaderCommitter()
{ }

TFuture<TMutationResponse> TLeaderCommitter::Commit(TMutationRequest&& request)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    NTracing::TNullTraceContextGuard traceContextGuard;

    auto keptResponse = DecoratedAutomaton_->TryBeginKeptRequest(request);
    if (keptResponse) {
        return keptResponse;
    }

    auto timestamp = GetInstant();

    if (LoggingSuspended_) {
        auto& pendingMutation = PendingMutations_.emplace_back(
            timestamp,
            std::move(request),
            traceContextGuard.GetOldTraceContext());
        return pendingMutation.CommitPromise;
    }

    auto commitFuture = LogLeaderMutation(
        timestamp,
        std::move(request),
        traceContextGuard.GetOldTraceContext());

    if (DecoratedAutomaton_->GetRecordCountSinceLastCheckpoint() >= Config_->MaxChangelogRecordCount) {
        YT_LOG_INFO("Requesting checkpoint due to record count limit (RecordCountSinceLastCheckpoint: %v, MaxChangelogRecordCount: %v)",
            DecoratedAutomaton_->GetRecordCountSinceLastCheckpoint(),
            Config_->MaxChangelogRecordCount);
        CheckpointNeeded_.Fire(false);
    } else if (DecoratedAutomaton_->GetDataSizeSinceLastCheckpoint() >= Config_->MaxChangelogDataSize)  {
        YT_LOG_INFO("Requesting checkpoint due to data size limit (DataSizeSinceLastCheckpoint: %v, MaxChangelogDataSize: %v)",
            DecoratedAutomaton_->GetDataSizeSinceLastCheckpoint(),
            Config_->MaxChangelogDataSize);
        CheckpointNeeded_.Fire(false);
    }

    return commitFuture;
}

void TLeaderCommitter::Flush()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (!CurrentBatch_) {
        return;
    }

    TBatchPtr currentBatch;
    std::swap(currentBatch, CurrentBatch_);
    PrevBatchQuorumFlushFuture_ = currentBatch->GetQuorumFlushFuture().AsVoid();
    BatchAlarm_->Disarm();

    auto committedVersion = DecoratedAutomaton_->GetCommittedVersion();

    EpochContext_->EpochControlInvoker->Invoke(
        BIND(&TBatch::Flush, std::move(currentBatch), committedVersion));
}

TFuture<void> TLeaderCommitter::GetQuorumFlushFuture()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return CurrentBatch_
        ? CurrentBatch_->GetQuorumFlushFuture().AsVoid()
        : PrevBatchQuorumFlushFuture_;
}

void TLeaderCommitter::DoSuspendLogging()
{
    YT_VERIFY(PendingMutations_.empty());
}

void TLeaderCommitter::DoResumeLogging()
{
    for (auto& pendingMutation : PendingMutations_) {
        auto commitFuture = LogLeaderMutation(
            pendingMutation.Timestamp,
            std::move(pendingMutation.Request),
            std::move(pendingMutation.TraceContext));
        pendingMutation.CommitPromise.SetFrom(commitFuture);
    }
    PendingMutations_.clear();

    BatchAlarm_->Check();
}

void TLeaderCommitter::Stop()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto error = TError(NRpc::EErrorCode::Unavailable, "Hydra peer has stopped");
    for (const auto& mutation : PendingMutations_) {
        mutation.CommitPromise.Set(error);
    }
}

TFuture<TMutationResponse> TLeaderCommitter::LogLeaderMutation(
    TInstant timestamp,
    TMutationRequest&& request,
    NTracing::TTraceContextPtr traceContext)
{
    TSharedRef recordData;
    TFuture<void> localFlushFuture;
    const auto& loggedMutation = DecoratedAutomaton_->LogLeaderMutation(
        timestamp,
        std::move(request),
        std::move(traceContext),
        &recordData,
        &localFlushFuture);

    AddToBatch(
        loggedMutation,
        std::move(recordData),
        std::move(localFlushFuture));

    return loggedMutation.LocalCommitPromise;
}

void TLeaderCommitter::AddToBatch(
    const TDecoratedAutomaton::TPendingMutation& pendingMutation,
    TSharedRef recordData,
    TFuture<void> localFlushFuture)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto batch = GetOrCreateBatch(pendingMutation.Version);
    batch->AddMutation(
        pendingMutation,
        std::move(recordData),
        std::move(localFlushFuture));

    BatchAlarm_->Check();

    if (batch->GetMutationCount() >= Config_->MaxCommitBatchRecordCount) {
        Flush();
    }
}

TLeaderCommitter::TBatchPtr TLeaderCommitter::GetOrCreateBatch(TVersion version)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (!CurrentBatch_) {
        CurrentBatch_ = New<TBatch>(this, version);
        CurrentBatch_->GetQuorumFlushFuture().Subscribe(
            BIND(&TLeaderCommitter::OnBatchCommitted, MakeWeak(this))
                .Via(EpochContext_->EpochUserAutomatonInvoker));

        BatchAlarm_->Arm(
            BIND(&TLeaderCommitter::Flush, MakeWeak(this)),
            Config_->MaxCommitBatchDelay);
    }

    return CurrentBatch_;
}

void TLeaderCommitter::OnBatchCommitted(const TErrorOr<TVersion>& errorOrVersion)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (!errorOrVersion.IsOK()) {
        return;
    }

    auto committedVersion = errorOrVersion.Value();
    DecoratedAutomaton_->CommitMutations(committedVersion, true);

    BatchAlarm_->Check();
}

void TLeaderCommitter::OnAutoSnapshotCheck()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (DecoratedAutomaton_->GetState() == EPeerState::Leading &&
        TInstant::Now() > DecoratedAutomaton_->GetSnapshotBuildDeadline())
    {
        YT_LOG_INFO("Requesting periodic snapshot (SnapshotBuildPeriod: %v, SnapshotBuildSplay: %v)",
            Config_->SnapshotBuildPeriod,
            Config_->SnapshotBuildSplay);
        CheckpointNeeded_.Fire(true);
    }
}

void TLeaderCommitter::FireCommitFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    CommitFailed_.Fire(error);
}

////////////////////////////////////////////////////////////////////////////////

TFollowerCommitter::TFollowerCommitter(
    TDistributedHydraManagerConfigPtr config,
    const TDistributedHydraManagerOptions& options,
    TDecoratedAutomatonPtr decoratedAutomaton,
    TEpochContext* epochContext,
    NLogging::TLogger logger,
    NProfiling::TProfiler profiler)
    : TCommitterBase(
        std::move(config),
        options,
        std::move(decoratedAutomaton),
        epochContext,
        std::move(logger),
        std::move(profiler))
{ }

TFuture<void> TFollowerCommitter::AcceptMutations(
    TVersion expectedVersion,
    const std::vector<TSharedRef>& recordsData)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (LoggingSuspended_) {
        TPendingMutation pendingMutation;
        pendingMutation.RecordsData = recordsData;
        pendingMutation.ExpectedVersion = expectedVersion;
        pendingMutation.Promise = NewPromise<void>();
        PendingMutations_.push_back(pendingMutation);
        return pendingMutation.Promise;
    }

    return DoAcceptMutations(expectedVersion, recordsData);
}

TFuture<void> TFollowerCommitter::DoAcceptMutations(
    TVersion expectedVersion,
    const std::vector<TSharedRef>& recordsData)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto currentVersion = DecoratedAutomaton_->GetLoggedVersion();
    if (currentVersion != expectedVersion) {
        return MakeFuture(TError(
            NHydra::EErrorCode::OutOfOrderMutations,
            "Out-of-order mutations received by follower: expected %v, actual %v",
            expectedVersion,
            currentVersion));
    }

    auto result = VoidFuture;
    int recordsCount = static_cast<int>(recordsData.size());
    for (int index = 0; index < recordsCount; ++index) {
        DecoratedAutomaton_->LogFollowerMutation(
            recordsData[index],
            index == recordsCount - 1 ? &result : nullptr);
    }
    return result;
}

void TFollowerCommitter::DoSuspendLogging()
{
    YT_VERIFY(PendingMutations_.empty());
}

void TFollowerCommitter::DoResumeLogging()
{
    for (const auto& pendingMutation : PendingMutations_) {
        auto future = DoAcceptMutations(pendingMutation.ExpectedVersion, pendingMutation.RecordsData);
        pendingMutation.Promise.SetFrom(std::move(future));
    }
    PendingMutations_.clear();
}

TFuture<TMutationResponse> TFollowerCommitter::Forward(TMutationRequest&& request)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto channel = CellManager_->GetPeerChannel(EpochContext_->LeaderId);
    YT_VERIFY(channel);

    THydraServiceProxy proxy(channel);
    proxy.SetDefaultTimeout(Config_->CommitForwardingRpcTimeout);

    auto req = proxy.CommitMutation();
    req->set_type(request.Type);
    req->set_reign(request.Reign);
    if (request.MutationId) {
        ToProto(req->mutable_mutation_id(), request.MutationId);
        req->set_retry(request.Retry);
    }
    req->Attachments().push_back(request.Data);

    return req->Invoke().Apply(BIND([] (const THydraServiceProxy::TErrorOrRspCommitMutationPtr& rspOrError) {
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error forwarding mutation to leader");
        const auto& rsp = rspOrError.Value();
        return TMutationResponse{
            EMutationResponseOrigin::LeaderForwarding,
            TSharedRefArray(rsp->Attachments(), TSharedRefArray::TMoveParts{})
        };
    }));
}

void TFollowerCommitter::Stop()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto error = TError(NRpc::EErrorCode::Unavailable, "Hydra peer has stopped");
    for (const auto& pendingMutation : PendingMutations_) {
        pendingMutation.Promise.Set(error);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
