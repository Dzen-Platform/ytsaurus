#include "dry_run_hydra_manager.h"

#include "decorated_automaton.h"
#include "private.h"

#include <yt/yt/server/lib/hydra_common/changelog.h>
#include <yt/yt/server/lib/hydra_common/config.h>
#include <yt/yt/server/lib/hydra_common/private.h>
#include <yt/yt/server/lib/hydra_common/snapshot.h>
#include <yt/yt/server/lib/hydra_common/state_hash_checker.h>

#include <yt/yt/server/lib/election/election_manager.h>

#include <yt/yt/core/logging/log_manager.h>

namespace NYT::NHydra2 {

using namespace NHydra;

using namespace NConcurrency;
using namespace NElection;
using namespace NLogging;
using namespace NProfiling;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TDryRunHydraManager
    : public IDryRunHydraManager
{
public:
    class TElectionCallbacks
        : public IElectionCallbacks
    {
    public:
        void OnStartLeading(NElection::TEpochContextPtr /*epochContext*/) override
        {
            YT_UNIMPLEMENTED();
        }

        void OnStopLeading(const TError& /*error*/) override
        {
            YT_UNIMPLEMENTED();
        }

        void OnStartFollowing(NElection::TEpochContextPtr /*epochContext*/) override
        {
            YT_UNIMPLEMENTED();
        }

        void OnStopFollowing(const TError& /*error*/) override
        {
            YT_UNIMPLEMENTED();
        }

        void OnStopVoting(const TError& /*error*/) override
        {
            YT_UNIMPLEMENTED();
        }

        TPeerPriority GetPriority() override
        {
            YT_UNIMPLEMENTED();
        }

        TString FormatPriority(TPeerPriority /*priority*/) override
        {
            YT_UNIMPLEMENTED();
        }
    };

    TDryRunHydraManager(
        TDistributedHydraManagerConfigPtr config,
        IInvokerPtr controlInvoker,
        IInvokerPtr automatonInvoker,
        IAutomatonPtr automaton,
        ISnapshotStorePtr snapshotStore,
        const TDistributedHydraManagerOptions& options,
        TCellManagerPtr cellManager)
        : Config_(New<TConfigWrapper>(config))
        , ControlInvoker_(std::move(controlInvoker))
        , AutomatonInvoker_(std::move(automatonInvoker))
        , SnapshotStore_(std::move(snapshotStore))
        , Options_(options)
        , StateHashChecker_(New<TStateHashChecker>(Config_->Get()->MaxStateHashCheckerEntryCount, HydraLogger))
        , Profiler_(TProfiler())
        , Logger(TLogger("DryRun"))
        , CellManager_(std::move(cellManager))
        , DecoratedAutomaton_(New<TDecoratedAutomaton>(
            Config_,
            Options_,
            automaton,
            AutomatonInvoker_,
            ControlInvoker_,
            SnapshotStore_,
            StateHashChecker_,
            HydraLogger,
            Profiler_))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(ControlInvoker_, ControlThread);
        VERIFY_INVOKER_THREAD_AFFINITY(AutomatonInvoker_, AutomatonThread);
    }

    void DryRunLoadSnapshot(const NHydra::ISnapshotReaderPtr& reader) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (!reader) {
            // Recover using changelogs only.
            YT_LOG_INFO("Not using snapshots for dry run recovery");
            return;
        }

        YT_LOG_INFO("Dry run instance started recovery using snapshot");

        WaitFor(reader->Open())
            .ThrowOnError();

        auto params = reader->GetParams();
        const auto& meta = params.Meta;

        DecoratedAutomaton_->LoadSnapshot(
            InvalidSegmentId,
            meta.last_mutation_term(),
            TVersion(meta.last_segment_id(), meta.last_record_id()),
            meta.sequence_number(),
            meta.random_seed(),
            meta.state_hash(),
            FromProto<TInstant>(meta.timestamp()),
            std::move(reader));

        YT_LOG_INFO("Checking invariants");

        DecoratedAutomaton_->CheckInvariants();

        YT_LOG_INFO("Successfully finished loading snapshot in dry run mode");
    }

    void DryRunReplayChangelog(IChangelogPtr changelog) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_INFO("Replaying changelog (RecordCount: %v)",
            changelog->GetRecordCount());

        DryRunStartLeading();

        int currentRecordId = 0;
        while (currentRecordId < changelog->GetRecordCount()) {
            YT_LOG_INFO("Started reading changelog records (FirstRecordId: %v)",
                currentRecordId);

            auto asyncRecordsData = changelog->Read(
                currentRecordId,
                Max<int>(),
                Config_->Get()->MaxChangelogBytesPerRequest);
            auto recordsData = WaitFor(asyncRecordsData)
                .ValueOrThrow();
            auto recordsRead = std::ssize(recordsData);

            YT_LOG_INFO("Finished reading changelog records (RecordIds: %v-%v)",
                currentRecordId,
                currentRecordId + recordsRead - 1);

            YT_LOG_INFO("Applying changelog records (RecordIds: %v-%v)",
                currentRecordId,
                currentRecordId + recordsRead - 1);

            for (const auto& recordData : recordsData)  {
                DecoratedAutomaton_->ApplyMutationDuringRecovery(recordData);
            }

            currentRecordId += recordsRead;
        }

        YT_LOG_INFO("Changelog replayed (ChangelogId: %v)",
            changelog->GetId());
    }

    void DryRunBuildSnapshot() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DryRunStartLeading();

        YT_LOG_INFO("Started building snapshot in dry run mode");
        auto sequenceNuber = DecoratedAutomaton_->GetSequenceNumber();
        auto nextSnapshotId = DecoratedAutomaton_->GetAutomatonVersion().SegmentId + 1;
        WaitFor(DecoratedAutomaton_->BuildSnapshot(nextSnapshotId, sequenceNuber))
            .ThrowOnError();
    }

    void DryRunShutdown() override
    {
        YT_LOG_INFO("Dry run hydra instance is shutting down");
        TLogManager::Get()->Shutdown();
    }


    // Stuff from ISimpleHydraManager
    TFuture<NHydra::TMutationResponse> CommitMutation(NHydra::TMutationRequest&& /*request*/) override
    {
        YT_UNIMPLEMENTED();
    }

    bool IsMutationLoggingEnabled() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return !IsRecovery() || Config_->Get()->ForceMutationLogging;
    }

    NHydra::TReign GetCurrentReign() override
    {
        YT_UNIMPLEMENTED();
    }

    NHydra::EPeerState GetAutomatonState() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedAutomaton_->GetState();
    }

    bool IsActiveLeader() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedAutomaton_->GetState() == EPeerState::Leading && LeaderRecovered_;
    }

    bool IsActiveFollower() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedAutomaton_->GetState() == EPeerState::Following && FollowerRecovered_;
    }

    TCancelableContextPtr GetAutomatonCancelableContext() const override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return AutomatonEpochContext_ ? AutomatonEpochContext_->CancelableContext : nullptr;
    }

    TEpochId GetAutomatonEpochId() const override
    {
        YT_UNIMPLEMENTED();
    }

    TFuture<void> Reconfigure(TDynamicDistributedHydraManagerConfigPtr /*config*/) override
    {
        YT_UNIMPLEMENTED();
    }

    DEFINE_SIGNAL_OVERRIDE(void(), StartLeading);
    DEFINE_SIGNAL_OVERRIDE(void(), AutomatonLeaderRecoveryComplete);
    DEFINE_SIGNAL_OVERRIDE(void(), ControlLeaderRecoveryComplete);
    DEFINE_SIGNAL_OVERRIDE(void(), LeaderActive);
    DEFINE_SIGNAL_OVERRIDE(void(), StopLeading);
    DEFINE_SIGNAL_OVERRIDE(void(), StartFollowing);
    DEFINE_SIGNAL_OVERRIDE(void(), AutomatonFollowerRecoveryComplete);
    DEFINE_SIGNAL_OVERRIDE(void(), ControlFollowerRecoveryComplete);
    DEFINE_SIGNAL_OVERRIDE(void(), StopFollowing);

    // Stuff from IHydraManager
    void Initialize() override
    {
        YT_UNIMPLEMENTED();
    }

    TFuture<void> Finalize() override
    {
        YT_UNIMPLEMENTED();
    }

    NElection::IElectionCallbacksPtr GetElectionCallbacks() override
    {
        return New<TElectionCallbacks>();
    }

    NHydra::EPeerState GetControlState() const override
    {
        YT_UNIMPLEMENTED();
    }

    NHydra::TVersion GetAutomatonVersion() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedAutomaton_->GetAutomatonVersion();
    }

    IInvokerPtr CreateGuardedAutomatonInvoker(IInvokerPtr underlyingInvoker) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedAutomaton_->CreateGuardedUserInvoker(underlyingInvoker);
    }

    TCancelableContextPtr GetControlCancelableContext() const override
    {
        YT_UNIMPLEMENTED();
    }

    TFuture<void> SyncWithLeader() override
    {
        YT_UNIMPLEMENTED();
    }

    TFuture<int> BuildSnapshot(bool /*setReadOnly*/, bool /*waitForSnapshotCompletion*/) override
    {
        YT_UNIMPLEMENTED();
    }

    NYson::TYsonProducer GetMonitoringProducer() override
    {
        YT_UNIMPLEMENTED();
    }

    NElection::TPeerIdSet GetAlivePeerIds() override
    {
        YT_UNIMPLEMENTED();
    }

    bool GetReadOnly() const override
    {
        YT_UNIMPLEMENTED();
    }

    DEFINE_SIGNAL_OVERRIDE(TFuture<void>(), LeaderLeaseCheck);

    // Stuff from IDistributedHydraManager
    NHydra::TDistributedHydraManagerDynamicOptions GetDynamicOptions() const override
    {
        YT_UNIMPLEMENTED();
    }

    void SetDynamicOptions(const NHydra::TDistributedHydraManagerDynamicOptions& /*options*/) override
    {
        YT_UNIMPLEMENTED();
    }

private:
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    const TConfigWrapperPtr Config_;
    const IInvokerPtr ControlInvoker_;
    const IInvokerPtr AutomatonInvoker_;
    const ISnapshotStorePtr SnapshotStore_;
    const TDistributedHydraManagerOptions Options_;
    const TStateHashCheckerPtr StateHashChecker_;

    const TProfiler Profiler_;

    const TLogger Logger;

    TCellManagerPtr CellManager_;

    TDecoratedAutomatonPtr DecoratedAutomaton_;

    TEpochContextPtr AutomatonEpochContext_;

    std::atomic<bool> LeaderRecovered_ = false;
    std::atomic<bool> FollowerRecovered_ = false;

    bool StartedLeading_ = false;

    void DryRunStartLeading()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // This only needs to be called once.
        if (StartedLeading_) {
            return;
        }

        YT_LOG_INFO("Mocking leading start");

        YT_VERIFY(!AutomatonEpochContext_);

        AutomatonEpochContext_ = New<TEpochContext>();
        AutomatonEpochContext_->CancelableContext = New<TCancelableContext>();
        AutomatonEpochContext_->CellManager = CellManager_;

        DecoratedAutomaton_->OnStartLeading(AutomatonEpochContext_);

        StartLeading_.Fire();

        StartedLeading_ = true;
    }
};

DEFINE_REFCOUNTED_TYPE(TDryRunHydraManager)

////////////////////////////////////////////////////////////////////////////////

NHydra::IHydraManagerPtr CreateDryRunHydraManager(
    TDistributedHydraManagerConfigPtr config,
    IInvokerPtr controlInvoker,
    IInvokerPtr automatonInvoker,
    IAutomatonPtr automaton,
    ISnapshotStorePtr snapshotStore,
    const TDistributedHydraManagerOptions& options,
    TCellManagerPtr cellManager)
{
    return New<TDryRunHydraManager>(
        config,
        controlInvoker,
        automatonInvoker,
        automaton,
        snapshotStore,
        options,
        cellManager);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra2
