#pragma once

#include "private.h"
#include "mutation_context.h"
#include "distributed_hydra_manager.h"

#include <core/misc/ref.h>
#include <core/misc/ring_queue.h>

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/delayed_executor.h>

#include <core/actions/future.h>
#include <core/actions/cancelable_context.h>

#include <core/rpc/public.h>

#include <core/logging/log.h>

#include <ytlib/hydra/version.h>
#include <ytlib/hydra/hydra_manager.pb.h>

#include <server/election/public.h>

#include <atomic>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TEpochContext
    : public TRefCounted
{
    IChangelogStorePtr ChangelogStore;
    TVersion ReachableVersion;

    IInvokerPtr EpochSystemAutomatonInvoker;
    IInvokerPtr EpochUserAutomatonInvoker;
    IInvokerPtr EpochControlInvoker;
    TCheckpointerPtr Checkpointer;
    TLeaderRecoveryPtr LeaderRecovery;
    TFollowerRecoveryPtr FollowerRecovery;
    TLeaderCommitterPtr LeaderCommitter;
    TFollowerCommitterPtr FollowerCommitter;
    TLeaseTrackerPtr LeaseTracker;

    std::atomic<bool> Restarting = {false};

    TPromise<void> ActiveUpstreamSyncPromise;
    TPromise<void> PendingUpstreamSyncPromise;
    bool UpstreamSyncDeadlineReached = false;

    TNullable<TVersion> LeaderSyncVersion;
    TPromise<void> LeaderSyncPromise;

    TPeerId LeaderId = InvalidPeerId;
    TEpochId EpochId;
    TCancelableContextPtr CancelableContext = New<TCancelableContext>();
};

DEFINE_REFCOUNTED_TYPE(TEpochContext)

////////////////////////////////////////////////////////////////////////////////

class TSystemLockGuard
    : private TNonCopyable
{
public:
    TSystemLockGuard();
    TSystemLockGuard(TSystemLockGuard&& other);
    ~TSystemLockGuard();

    TSystemLockGuard& operator = (TSystemLockGuard&& other);

    void Release();

    explicit operator bool() const;

    static TSystemLockGuard Acquire(TDecoratedAutomatonPtr automaton);

private:
    explicit TSystemLockGuard(TDecoratedAutomatonPtr automaton);

    TDecoratedAutomatonPtr Automaton_;

};

////////////////////////////////////////////////////////////////////////////////

class TUserLockGuard
    : private TNonCopyable
{
public:
    TUserLockGuard();
    TUserLockGuard(TUserLockGuard&& other);
    ~TUserLockGuard();

    TUserLockGuard& operator = (TUserLockGuard&& other);

    void Release();

    explicit operator bool() const;

    static TUserLockGuard TryAcquire(TDecoratedAutomatonPtr automaton);

private:
    explicit TUserLockGuard(TDecoratedAutomatonPtr automaton);

    TDecoratedAutomatonPtr Automaton_;

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton
    : public TRefCounted
{
public:
    TDecoratedAutomaton(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        IAutomatonPtr automaton,
        IInvokerPtr automatonInvoker,
        IInvokerPtr controlInvoker,
        ISnapshotStorePtr snapshotStore,
        const TDistributedHydraManagerOptions& options);

    void Initialize();
    void OnStartLeading(TEpochContextPtr epochContext);
    void OnLeaderRecoveryComplete();
    void OnStopLeading();
    void OnStartFollowing(TEpochContextPtr epochContext);
    void OnFollowerRecoveryComplete();
    void OnStopFollowing();

    DEFINE_BYVAL_RO_PROPERTY(EPeerState, State);

    IInvokerPtr CreateGuardedUserInvoker(IInvokerPtr underlyingInvoker);
    IInvokerPtr GetDefaultGuardedUserInvoker();
    IInvokerPtr GetSystemInvoker();

    TVersion GetLoggedVersion() const;
    void SetLoggedVersion(TVersion version);

    void SetChangelog(IChangelogPtr changelog);

    i64 GetLoggedDataSize() const;
    TInstant GetLastSnapshotTime() const;

    TVersion GetAutomatonVersion() const;
    void RotateAutomatonVersion(int segmentId);

    TVersion GetCommittedVersion() const;

    void LoadSnapshot(TVersion version, NConcurrency::IAsyncZeroCopyInputStreamPtr reader);

    void ApplyMutationDuringRecovery(const TSharedRef& recordData);

    void LogLeaderMutation(
        const TMutationRequest& request,
        TSharedRef* recordData,
        TFuture<void>* localFlushResult,
        TFuture<TMutationResponse>* commitResult);

    void CancelPendingLeaderMutations(const TError& error);

    void LogFollowerMutation(
        const TSharedRef& recordData,
        TFuture<void>* localFlushResult);

    TFuture<TRemoteSnapshotParams> BuildSnapshot();

    TFuture<void> RotateChangelog();

    void CommitMutations(TVersion version, bool mayYield);

    bool HasReadyMutations() const;

private:
    friend class TUserLockGuard;
    friend class TSystemLockGuard;

    class TGuardedUserInvoker;
    class TSystemInvoker;
    class TSnapshotBuilderBase;
    class TForkSnapshotBuilder;
    class TSwitchableSnapshotWriter;
    class TNoForkSnapshotBuilder;

    const TDistributedHydraManagerConfigPtr Config_;
    const NElection::TCellManagerPtr CellManager_;
    const IAutomatonPtr Automaton_;
    const IInvokerPtr AutomatonInvoker_;
    const IInvokerPtr DefaultGuardedUserInvoker_;
    const IInvokerPtr ControlInvoker_;
    const IInvokerPtr SystemInvoker_;
    const ISnapshotStorePtr SnapshotStore_;
    const TDistributedHydraManagerOptions Options_;

    std::atomic<int> UserLock_ = {0};
    std::atomic<int> SystemLock_ = {0};

    TEpochContextPtr EpochContext_;
    IChangelogPtr Changelog_;

    // AutomatonVersion_ <= CommittedVersion_ <= LoggedVersion_
    std::atomic<TVersion> LoggedVersion_;
    std::atomic<TVersion> AutomatonVersion_;
    std::atomic<TVersion> CommittedVersion_;

    TVersion SnapshotVersion_;
    TPromise<TRemoteSnapshotParams> SnapshotParamsPromise_;
    std::atomic_flag BuildingSnapshot_;
    TInstant LastSnapshotTime_;

    struct TPendingMutation
    {
        TVersion Version;
        TMutationRequest Request;
        TInstant Timestamp;
        ui64 RandomSeed;
        TPromise<TMutationResponse> CommitPromise;
    };

    NProto::TMutationHeader MutationHeader_; // pooled instance
    TRingQueue<TPendingMutation> PendingMutations_;

    NProfiling::TAggregateCounter BatchCommitTimeCounter_;

    NLogging::TLogger Logger;


    void RotateAutomatonVersionIfNeeded(TVersion mutationVersion);
    void DoApplyMutation(TMutationContext* context);

    bool TryAcquireUserLock();
    void ReleaseUserLock();
    void AcquireSystemLock();
    void ReleaseSystemLock();

    void StartEpoch(TEpochContextPtr epochContext);
    void StopEpoch();

    void DoRotateChangelog();

    void ApplyPendingMutations(bool mayYield);

    TFuture<void> SaveSnapshot(NConcurrency::IAsyncOutputStreamPtr writer);
    void MaybeStartSnapshotBuilder();

    bool IsRecovery();

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TDecoratedAutomaton)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
