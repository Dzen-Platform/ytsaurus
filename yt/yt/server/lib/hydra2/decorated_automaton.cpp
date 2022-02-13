#include "decorated_automaton.h"

#include <yt/yt/server/lib/hydra_common/automaton.h>
#include <yt/yt/server/lib/hydra_common/changelog.h>
#include <yt/yt/server/lib/hydra_common/config.h>
#include <yt/yt/server/lib/hydra_common/serialize.h>
#include <yt/yt/server/lib/hydra_common/snapshot.h>
#include <yt/yt/server/lib/hydra_common/state_hash_checker.h>
#include <yt/yt/server/lib/hydra_common/snapshot_discovery.h>

#include <yt/yt/server/lib/misc/fork_executor.h>

#include <yt/yt/ytlib/election/cell_manager.h>

#include <yt/yt/ytlib/hydra/proto/hydra_manager.pb.h>
#include <yt/yt/ytlib/hydra/proto/hydra_service.pb.h>

#include <yt/yt/core/actions/invoker_detail.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/delayed_executor.h>

#include <yt/yt/core/misc/blob.h>
#include <yt/yt/core/misc/proc.h>
#include <yt/yt/core/misc/finally.h>

#include <yt/yt/core/net/connection.h>

#include <yt/yt/core/utilex/random.h>

#include <yt/yt/library/process/pipe.h>

#include <yt/yt/core/profiling/timing.h>

#include <yt/yt/core/rpc/response_keeper.h>

#include <yt/yt/core/logging/log_manager.h>
#include <yt/yt/core/logging/logger_owner.h>

#include <yt/yt/core/tracing/trace_context.h>

#include <util/random/random.h>

#include <util/system/file.h>
#include <util/system/spinlock.h>

#include <algorithm> // for std::max

namespace NYT::NHydra2 {

using namespace NConcurrency;
using namespace NElection;
using namespace NHydra;
// using namespace NHydra2::NProto;
using namespace NLogging;
using namespace NPipes;
using namespace NProfiling;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

static const i64 SnapshotTransferBlockSize = 1_MB;

////////////////////////////////////////////////////////////////////////////////

TPendingMutation::TPendingMutation(
    TVersion version,
    NHydra::TMutationRequest&& request,
    TInstant timestamp,
    ui64 randomSeed,
    ui64 prevRandomSeed,
    i64 sequenceNumber,
    int term,
    TSharedRef serializedMutation,
    TPromise<NHydra::TMutationResponse> promise)
    : Version(version)
    , Request(request)
    , Timestamp(timestamp)
    , RandomSeed(randomSeed)
    , PrevRandomSeed(prevRandomSeed)
    , SequenceNumber(sequenceNumber)
    , Term(term)
    , RecordData(serializedMutation)
    , LocalCommitPromise(std::move(promise))
{ }

////////////////////////////////////////////////////////////////////////////////

TSystemLockGuard::TSystemLockGuard(TSystemLockGuard&& other)
    : Automaton_(std::move(other.Automaton_))
{ }

TSystemLockGuard::~TSystemLockGuard()
{
    Release();
}

TSystemLockGuard& TSystemLockGuard::operator=(TSystemLockGuard&& other)
{
    Release();
    Automaton_ = std::move(other.Automaton_);
    return *this;
}

void TSystemLockGuard::Release()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (Automaton_) {
        Automaton_->ReleaseSystemLock();
        Automaton_.Reset();
    }
}

TSystemLockGuard::operator bool() const
{
    return static_cast<bool>(Automaton_);
}

TSystemLockGuard TSystemLockGuard::Acquire(TDecoratedAutomatonPtr automaton)
{
    automaton->AcquireSystemLock();
    return TSystemLockGuard(std::move(automaton));
}

TSystemLockGuard::TSystemLockGuard(TDecoratedAutomatonPtr automaton)
    : Automaton_(std::move(automaton))
{ }

////////////////////////////////////////////////////////////////////////////////

TUserLockGuard::TUserLockGuard(TUserLockGuard&& other)
    : Automaton_(std::move(other.Automaton_))
{ }

TUserLockGuard::~TUserLockGuard()
{
    Release();
}

TUserLockGuard& TUserLockGuard::operator=(TUserLockGuard&& other)
{
    Release();
    Automaton_ = std::move(other.Automaton_);
    return *this;
}

void TUserLockGuard::Release()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (Automaton_) {
        Automaton_->ReleaseUserLock();
        Automaton_.Reset();
    }
}

TUserLockGuard::operator bool() const
{
    return static_cast<bool>(Automaton_);
}

TUserLockGuard TUserLockGuard::TryAcquire(TDecoratedAutomatonPtr automaton)
{
    return automaton->TryAcquireUserLock()
        ? TUserLockGuard(std::move(automaton))
        : TUserLockGuard();
}

TUserLockGuard::TUserLockGuard(TDecoratedAutomatonPtr automaton)
    : Automaton_(std::move(automaton))
{ }

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TSystemInvoker
    : public TInvokerWrapper
{
public:
    explicit TSystemInvoker(TDecoratedAutomaton* decoratedAutomaton)
        : TInvokerWrapper(decoratedAutomaton->AutomatonInvoker_)
        , Owner_(decoratedAutomaton)
    { }

    void Invoke(TClosure callback) override
    {
        auto lockGuard = TSystemLockGuard::Acquire(Owner_);

        auto doInvoke = [=, this_ = MakeStrong(this), callback = std::move(callback)] (TSystemLockGuard /*lockGuard*/) {
            TCurrentInvokerGuard currentInvokerGuard(this_);
            callback.Run();
        };

        UnderlyingInvoker_->Invoke(BIND(doInvoke, Passed(std::move(lockGuard))));
    }

private:
    TDecoratedAutomaton* const Owner_;
};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TGuardedUserInvoker
    : public TInvokerWrapper
{
public:
    TGuardedUserInvoker(
        TDecoratedAutomatonPtr decoratedAutomaton,
        IInvokerPtr underlyingInvoker)
        : TInvokerWrapper(std::move(underlyingInvoker))
        , Owner_(decoratedAutomaton)
    { }

    void Invoke(TClosure callback) override
    {
        auto lockGuard = TUserLockGuard::TryAcquire(Owner_);
        if (!lockGuard) {
            return;
        }

        auto doInvoke = [=, this_ = MakeStrong(this), callback = std::move(callback)] () {
            if (Owner_->GetState() != EPeerState::Leading &&
                Owner_->GetState() != EPeerState::Following)
                return;

            TCurrentInvokerGuard guard(this_);
            callback.Run();
        };

        UnderlyingInvoker_->Invoke(BIND(doInvoke));
    }

private:
    const TDecoratedAutomatonPtr Owner_;
};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TSnapshotBuilderBase
    : public virtual NLogging::TLoggerOwner
    , public virtual TRefCounted
{
public:
    explicit TSnapshotBuilderBase(TDecoratedAutomatonPtr owner)
        : Owner_(owner)
        , SequenceNumber_(Owner_->SequenceNumber_)
        , SnapshotId_(Owner_->NextSnapshotId_)
        , RandomSeed_(Owner_->RandomSeed_)
        , StateHash_(Owner_->StateHash_)
        , Timestamp_(Owner_->Timestamp_)
        , EpochContext_(Owner_->EpochContext_)
    {
        Logger = Owner_->Logger.WithTag("SnapshotId: %v", SnapshotId_);
    }

    ~TSnapshotBuilderBase()
    {
        ReleaseLock();
    }

    TFuture<TRemoteSnapshotParams> Run()
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        try {
            TryAcquireLock();

            NHydra::NProto::TSnapshotMeta meta;
            meta.set_sequence_number(SequenceNumber_);
            meta.set_random_seed(RandomSeed_);
            meta.set_state_hash(StateHash_);
            meta.set_timestamp(Timestamp_.GetValue());
            auto automatonVersion = Owner_->AutomatonVersion_.load();
            meta.set_last_segment_id(automatonVersion.SegmentId);
            meta.set_last_record_id(automatonVersion.RecordId);
            YT_VERIFY(Owner_->EpochContext_->Term >= Owner_->LastMutationTerm_);
            meta.set_last_mutation_term(Owner_->LastMutationTerm_);
            meta.set_term(Owner_->EpochContext_->Term);

            SnapshotWriter_ = Owner_->SnapshotStore_->CreateWriter(SnapshotId_, meta);

            return DoRun().Apply(
                BIND(&TSnapshotBuilderBase::OnFinished, MakeStrong(this))
                    .AsyncVia(GetHydraIOInvoker()));
        } catch (const std::exception& ex) {
            ReleaseLock();
            return MakeFuture<TRemoteSnapshotParams>(TError(ex));
        }
    }

    int GetSnapshotId() const
    {
        return SnapshotId_;
    }

protected:
    const TDecoratedAutomatonPtr Owner_;
    const i64 SequenceNumber_;
    const int SnapshotId_;
    const ui64 RandomSeed_;
    const ui64 StateHash_;
    const TInstant Timestamp_;
    const TEpochContextPtr EpochContext_;

    ISnapshotWriterPtr SnapshotWriter_;


    virtual TFuture<void> DoRun() = 0;

    void TryAcquireLock()
    {
        bool expected = false;
        if (!Owner_->BuildingSnapshot_.compare_exchange_strong(expected, true)) {
            THROW_ERROR_EXCEPTION("Cannot start building snapshot %v since another snapshot is still being constructed",
                SnapshotId_);
        }
        LockAcquired_ = true;

        YT_LOG_INFO("Snapshot builder lock acquired");
    }

    void ReleaseLock()
    {
        if (LockAcquired_) {
            auto delay = Owner_->Config_->BuildSnapshotDelay;
            if (delay != TDuration::Zero()) {
                YT_LOG_DEBUG("Working in testing mode, sleeping (BuildSnapshotDelay: %v)", delay);
                TDelayedExecutor::WaitForDuration(delay);
            }

            Owner_->BuildingSnapshot_.store(false);
            LockAcquired_ = false;

            YT_LOG_INFO("Snapshot builder lock released");
        }
    }

private:
    bool LockAcquired_ = false;


    TRemoteSnapshotParams OnFinished(const TError& error)
    {
        ReleaseLock();

        error.ThrowOnError();

        const auto& params = SnapshotWriter_->GetParams();

        TRemoteSnapshotParams remoteParams;
        remoteParams.PeerId = EpochContext_->CellManager->GetSelfPeerId();
        remoteParams.SnapshotId = SnapshotId_;
        static_cast<TSnapshotParams&>(remoteParams) = params;
        return remoteParams;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TForkSnapshotBuilder
    : public TSnapshotBuilderBase
    , public TForkExecutor
{
public:
    TForkSnapshotBuilder(TDecoratedAutomatonPtr owner, TForkCountersPtr counters)
        : TSnapshotBuilderBase(owner)
        , TForkExecutor(std::move(counters))
    { }

private:
    IAsyncInputStreamPtr InputStream_;
    std::unique_ptr<TFile> OutputFile_;

    TFuture<void> AsyncTransferResult_;


    TFuture<void> DoRun() override
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        auto pipe = TPipeFactory().Create();
        YT_LOG_INFO("Snapshot transfer pipe opened (Pipe: %v)",
            pipe);

        InputStream_ = pipe.CreateAsyncReader();
        OutputFile_ = std::make_unique<TFile>(FHANDLE(pipe.ReleaseWriteFD()));

        AsyncTransferResult_ = BIND(&TForkSnapshotBuilder::TransferLoop, MakeStrong(this))
            .AsyncVia(GetWatchdogInvoker())
            .Run();

        return Fork().Apply(
            BIND(&TForkSnapshotBuilder::OnFinished, MakeStrong(this))
                .AsyncVia(GetHydraIOInvoker()));
    }

    TDuration GetTimeout() const override
    {
        return Owner_->Config_->SnapshotBuildTimeout;
    }

    TDuration GetForkTimeout() const override
    {
        return Owner_->Config_->SnapshotForkTimeout;
    }

    void RunChild() override
    {
        CloseAllDescriptors({
            2, // stderr
            int(OutputFile_->GetHandle())
        });
        TUnbufferedFileOutput output(*OutputFile_);
        auto writer = CreateAsyncAdapter(&output);
        Owner_->SaveSnapshot(writer)
            .Get()
            .ThrowOnError();
        OutputFile_->Close();
    }

    void RunParent() override
    {
        OutputFile_->Close();
    }

    void Cleanup() override
    {
        ReleaseLock();
    }

    void TransferLoop()
    {
        YT_LOG_INFO("Snapshot transfer loop started");

        WaitFor(SnapshotWriter_->Open())
            .ThrowOnError();

        auto zeroCopyReader = CreateZeroCopyAdapter(InputStream_, SnapshotTransferBlockSize);
        auto zeroCopyWriter = CreateZeroCopyAdapter(SnapshotWriter_);

        TFuture<void> lastWriteResult;
        i64 size = 0;

        while (true) {
            auto block = WaitFor(zeroCopyReader->Read())
                .ValueOrThrow();

            if (!block)
                break;

            size += block.Size();
            lastWriteResult = zeroCopyWriter->Write(block);
        }

        if (lastWriteResult) {
            WaitFor(lastWriteResult)
                .ThrowOnError();
        }

        YT_LOG_INFO("Snapshot transfer loop completed (Size: %v)",
            size);
    }

    void OnFinished()
    {
        YT_LOG_INFO("Waiting for transfer loop to finish");
        WaitFor(AsyncTransferResult_)
            .ThrowOnError();
        YT_LOG_INFO("Transfer loop finished");

        YT_LOG_INFO("Waiting for snapshot writer to close");
        WaitFor(SnapshotWriter_->Close())
            .ThrowOnError();
        YT_LOG_INFO("Snapshot writer closed");
    }
};

////////////////////////////////////////////////////////////////////////////////

/*!
 *  The stream goes through the following sequence of states:
 *  1. initially it is created in sync mode
 *  2. then it is suspended
 *  3. then it is resumed in async mode
 *
 */
class TDecoratedAutomaton::TSwitchableSnapshotWriter
    : public IAsyncOutputStream
{
public:
    explicit TSwitchableSnapshotWriter(const NLogging::TLogger& logger)
        : Logger(logger)
    { }

    void Suspend()
    {
        auto guard = Guard(SpinLock_);
        SuspendedPromise_ = NewPromise<void>();
    }

    void ResumeAsAsync(IAsyncOutputStreamPtr underlyingStream)
    {
        auto guard = Guard(SpinLock_);
        auto suspendedPromise = SuspendedPromise_;
        SuspendedPromise_.Reset();
        UnderlyingStream_ = CreateZeroCopyAdapter(underlyingStream);
        for (const auto& syncBlock : SyncBlocks_) {
            ForwardBlock(syncBlock);
        }
        SyncBlocks_.clear();
        guard.Release();
        suspendedPromise.Set();
    }

    void Abort()
    {
        auto guard = Guard(SpinLock_);
        auto suspendedPromise = SuspendedPromise_;
        guard.Release();

        if (suspendedPromise) {
            suspendedPromise.TrySet(TError("Snapshot writer aborted"));
        }
    }

    TFuture<void> Close() override
    {
        auto guard = Guard(SpinLock_);
        return LastForwardResult_;
    }

    TFuture<void> Write(const TSharedRef& block) override
    {
        // NB: We are not allowed to store by-ref copies of #block, cf. #IAsyncOutputStream::Write.
        struct TBlockTag { };
        auto blockCopy = TSharedRef::MakeCopy<TBlockTag>(block);

        auto guard = Guard(SpinLock_);
        if (UnderlyingStream_) {
            YT_LOG_TRACE("Got async snapshot block (Size: %v)", blockCopy.Size());
            AsyncSize_ += block.Size();
            return ForwardBlock(blockCopy);
        } else {
            YT_LOG_TRACE("Got sync snapshot block (Size: %v)", blockCopy.Size());
            SyncBlocks_.push_back(blockCopy);
            SyncSize_ += block.Size();
            return SuspendedPromise_ ? SuspendedPromise_.ToFuture() : VoidFuture;
        }
    }

    i64 GetSyncSize() const
    {
        YT_VERIFY(UnderlyingStream_);
        return SyncSize_;
    }

    i64 GetAsyncSize() const
    {
        YT_VERIFY(UnderlyingStream_);
        return AsyncSize_;
    }

private:
    const NLogging::TLogger Logger;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);
    TPromise<void> SuspendedPromise_;
    i64 SyncSize_ = 0;
    i64 AsyncSize_ = 0;
    IAsyncZeroCopyOutputStreamPtr UnderlyingStream_;
    std::vector<TSharedRef> SyncBlocks_;
    TFuture<void> LastForwardResult_ = VoidFuture;


    TFuture<void> ForwardBlock(const TSharedRef& block)
    {
        return LastForwardResult_ = UnderlyingStream_->Write(block);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TNoForkSnapshotBuilder
    : public TSnapshotBuilderBase
{
public:
    using TSnapshotBuilderBase::TSnapshotBuilderBase;

    ~TNoForkSnapshotBuilder()
    {
        if (SwitchableSnapshotWriter_) {
            SwitchableSnapshotWriter_->Abort();
        }
    }

private:
    TIntrusivePtr<TSwitchableSnapshotWriter> SwitchableSnapshotWriter_;

    TFuture<void> AsyncOpenWriterResult_;
    TFuture<void> AsyncSaveSnapshotResult_;


    TFuture<void> DoRun() override
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        SwitchableSnapshotWriter_ = New<TSwitchableSnapshotWriter>(Logger);

        AsyncOpenWriterResult_ = SnapshotWriter_->Open();

        YT_LOG_INFO("Snapshot sync phase started");

        AsyncSaveSnapshotResult_ = Owner_->SaveSnapshot(SwitchableSnapshotWriter_);

        YT_LOG_INFO("Snapshot sync phase completed");

        SwitchableSnapshotWriter_->Suspend();

        return BIND(&TNoForkSnapshotBuilder::DoRunAsync, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run();
    }

    void DoRunAsync()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        WaitFor(AsyncOpenWriterResult_)
            .ThrowOnError();

        YT_LOG_INFO("Switching to async snapshot writer");

        SwitchableSnapshotWriter_->ResumeAsAsync(SnapshotWriter_);

        WaitFor(AsyncSaveSnapshotResult_)
            .ThrowOnError();

        YT_LOG_INFO("Snapshot async phase completed (SyncSize: %v, AsyncSize: %v)",
            SwitchableSnapshotWriter_->GetSyncSize(),
            SwitchableSnapshotWriter_->GetAsyncSize());

        WaitFor(SwitchableSnapshotWriter_->Close())
            .ThrowOnError();

        WaitFor(SnapshotWriter_->Close())
            .ThrowOnError();
    }
};

////////////////////////////////////////////////////////////////////////////////

TDecoratedAutomaton::TDecoratedAutomaton(
    TDistributedHydraManagerConfigPtr config,
    const TDistributedHydraManagerOptions& options,
    IAutomatonPtr automaton,
    IInvokerPtr automatonInvoker,
    IInvokerPtr controlInvoker,
    ISnapshotStorePtr snapshotStore,
    TStateHashCheckerPtr stateHashChecker,
    const NLogging::TLogger& logger,
    const NProfiling::TProfiler& profiler)
    : Logger(logger)
    , Config_(std::move(config))
    , Options_(options)
    , Automaton_(std::move(automaton))
    , AutomatonInvoker_(std::move(automatonInvoker))
    , DefaultGuardedUserInvoker_(CreateGuardedUserInvoker(AutomatonInvoker_))
    , ControlInvoker_(std::move(controlInvoker))
    , SystemInvoker_(New<TSystemInvoker>(this))
    , SnapshotStore_(std::move(snapshotStore))
    , StateHashChecker_(std::move(stateHashChecker))
    , BatchCommitTimer_(profiler.Timer("/batch_commit_time"))
    , SnapshotLoadTime_(profiler.TimeGauge("/snapshot_load_time"))
    , ForkCounters_(New<TForkCounters>(profiler))
{
    YT_VERIFY(Config_);
    YT_VERIFY(Automaton_);
    YT_VERIFY(ControlInvoker_);
    YT_VERIFY(SnapshotStore_);
    VERIFY_INVOKER_THREAD_AFFINITY(AutomatonInvoker_, AutomatonThread);
    VERIFY_INVOKER_THREAD_AFFINITY(ControlInvoker_, ControlThread);
}

void TDecoratedAutomaton::Initialize()
{
    AutomatonInvoker_->Invoke(BIND([=, this_ = MakeStrong(this)] () {
        Automaton_->Clear();
        Automaton_->SetZeroState();
    }));
}

void TDecoratedAutomaton::OnStartLeading(TEpochContextPtr epochContext)
{
    YT_VERIFY(State_ == EPeerState::Stopped);
    State_ = EPeerState::LeaderRecovery;
    StartEpoch(epochContext);
}

void TDecoratedAutomaton::OnLeaderRecoveryComplete()
{
    YT_VERIFY(State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Leading;
    UpdateSnapshotBuildDeadline();
}

void TDecoratedAutomaton::OnStopLeading()
{
    YT_VERIFY(State_ == EPeerState::Leading || State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Stopped;
    StopEpoch();
}

void TDecoratedAutomaton::OnStartFollowing(TEpochContextPtr epochContext)
{
    YT_VERIFY(State_ == EPeerState::Stopped);
    State_ = EPeerState::FollowerRecovery;
    StartEpoch(epochContext);
}

void TDecoratedAutomaton::OnFollowerRecoveryComplete()
{
    YT_VERIFY(State_ == EPeerState::FollowerRecovery);
    State_ = EPeerState::Following;
    UpdateSnapshotBuildDeadline();
}

void TDecoratedAutomaton::OnStopFollowing()
{
    YT_VERIFY(State_ == EPeerState::Following || State_ == EPeerState::FollowerRecovery);
    State_ = EPeerState::Stopped;
    StopEpoch();
}

IInvokerPtr TDecoratedAutomaton::CreateGuardedUserInvoker(IInvokerPtr underlyingInvoker)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return New<TGuardedUserInvoker>(this, underlyingInvoker);
}

IInvokerPtr TDecoratedAutomaton::GetDefaultGuardedUserInvoker()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return DefaultGuardedUserInvoker_;
}

IInvokerPtr TDecoratedAutomaton::GetSystemInvoker()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SystemInvoker_;
}

TFuture<void> TDecoratedAutomaton::SaveSnapshot(IAsyncOutputStreamPtr writer)
{
    // No affinity annotation here since this could have been called
    // from a forked process.

    // Context switches are not allowed during sync phase.
    TForbidContextSwitchGuard contextSwitchGuard;

    return Automaton_->SaveSnapshot(writer);
}

void TDecoratedAutomaton::LoadSnapshot(
    int snapshotId,
    int lastMutationTerm,
    TVersion version,
    i64 sequenceNumber,
    ui64 randomSeed,
    ui64 stateHash,
    TInstant timestamp,
    IAsyncZeroCopyInputStreamPtr reader)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    YT_LOG_INFO("Started loading snapshot (SnapshotId: %v)",
        snapshotId);

    TWallTimer timer;
    auto finally = Finally([&] {
        SnapshotLoadTime_.Update(timer.GetElapsedTime());
    });

    Automaton_->Clear();
    try {
        AutomatonVersion_ = TVersion(-1, -1);
        RandomSeed_ = 0;
        SequenceNumber_ = 0;
        StateHash_ = 0;
        Timestamp_ = {};
        {
            auto snapshotReign = Automaton_->LoadSnapshot(reader);

            // Snapshot preparation is a "mutation" that is executed before first mutation
            // in changelog.
            TVersion hydraContextVersion(snapshotId, -1);
            // NB: #randomSeed is used as a random seed for the first mutation
            // in changelog, so ad-hoc seed is used here.
            auto hydraContextRandomSeed = randomSeed;
            HashCombine(hydraContextRandomSeed, snapshotId);

            THydraContext hydraContext(
                hydraContextVersion,
                timestamp,
                hydraContextRandomSeed,
                snapshotReign);
            THydraContextGuard hydraContextGuard(&hydraContext);

            Automaton_->PrepareState();
        }
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Snapshot load failed; clearing state");
        Automaton_->Clear();
        throw;
    } catch (const TFiberCanceledException&) {
        YT_LOG_INFO("Snapshot load fiber was canceled");
        throw;
    } catch (...) {
        YT_LOG_ERROR("Snapshot load failed with an unknown error");
        throw;
    }

    YT_LOG_INFO("Finished loading snapshot");

    AutomatonVersion_ = version;
    RandomSeed_ = randomSeed;
    SequenceNumber_ = sequenceNumber;
    StateHash_ = stateHash;
    Timestamp_ = timestamp;
    // This protects us from building a snapshot with the same id twice.
    // If we join an active quorum and a leader is currently building a snapshot with id N,
    // we will be asked to recover to version (N - 1, M) may be using snapshot N (it might be already
    // built on some peers).
    // After recovery leader may still ask us to build snapshot N, but we already downloaded it from another peer,
    // so just refuse.
    LastSuccessfulSnapshotId_ = snapshotId;
    LastMutationTerm_ = lastMutationTerm;
}

void TDecoratedAutomaton::ValidateSnapshot(IAsyncZeroCopyInputStreamPtr reader)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    YT_VERIFY(State_ == EPeerState::Stopped);
    State_ = EPeerState::LeaderRecovery;

    LoadSnapshot(0, 0, {}, 0, 0, 0, TInstant{}, reader);
    Automaton_->CheckInvariants();

    YT_VERIFY(State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Stopped;
}

void TDecoratedAutomaton::ApplyMutationDuringRecovery(const TSharedRef& recordData)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    NHydra::NProto::TMutationHeader header;
    TSharedRef requestData;
    DeserializeMutationRecord(recordData, &header, &requestData);

    auto mutationVersion = TVersion(header.segment_id(), header.record_id());

    TMutationRequest request;
    request.Reign = header.reign();
    request.Type = header.mutation_type();
    request.MutationId = FromProto<TMutationId>(header.mutation_id());
    request.Data = std::move(requestData);

    TMutationContext mutationContext(
        AutomatonVersion_,
        request,
        FromProto<TInstant>(header.timestamp()),
        header.random_seed(),
        header.prev_random_seed(),
        header.sequence_number(),
        StateHash_);

    DoApplyMutation(&mutationContext, mutationVersion, header.term());
}

TFuture<TMutationResponse> TDecoratedAutomaton::TryBeginKeptRequest(const TMutationRequest& request)
{
    VERIFY_THREAD_AFFINITY_ANY();

    YT_VERIFY(State_ == EPeerState::Leading);

    if (!Options_.ResponseKeeper) {
        return TFuture<TMutationResponse>();
    }

    if (!request.MutationId) {
        return TFuture<TMutationResponse>();
    }

    auto asyncResponseData = Options_.ResponseKeeper->TryBeginRequest(request.MutationId, request.Retry);
    if (!asyncResponseData) {
        return TFuture<TMutationResponse>();
    }

    return asyncResponseData.Apply(BIND([] (const TSharedRefArray& data) {
        return TMutationResponse{
            EMutationResponseOrigin::ResponseKeeper,
            data
        };
    }));
}

TFuture<TRemoteSnapshotParams> TDecoratedAutomaton::BuildSnapshot(int snapshotId, i64 sequenceNumber)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    // TODO(aleksandra-zh): this should be considered a success.
    if (LastSuccessfulSnapshotId_ >= snapshotId) {
        TError error("Cannot build a snapshot %v because last built snapshot id %v is greater",
            snapshotId,
            LastSuccessfulSnapshotId_.load());
        YT_LOG_INFO(error, "Error building snapshot");
        return MakeFuture<TRemoteSnapshotParams>(error);
    }

    if (SequenceNumber_ > sequenceNumber) {
        TError error("Cannot build a snapshot %v from sequence number %v because automaton sequence number is greater %v",
            snapshotId,
            sequenceNumber,
            SequenceNumber_.load());
        YT_LOG_INFO(error, "Error building snapshot");
        return MakeFuture<TRemoteSnapshotParams>(error);
    }

    // We are already building this snapshot.
    if (NextSnapshotId_ == snapshotId) {
        YT_LOG_INFO("We are already building this snapshot (SnapshotId: %v)", NextSnapshotId_);
        return SnapshotParamsPromise_;
    }

    YT_VERIFY(NextSnapshotId_ < snapshotId);

    YT_LOG_INFO("Started building snapshot (SnapshotId: %v, SequenceNumber: %v)",
        snapshotId,
        sequenceNumber);

    SnapshotSequenceNumber_ = sequenceNumber;
    NextSnapshotId_ = snapshotId;
    SnapshotParamsPromise_ = NewPromise<TRemoteSnapshotParams>();

    MaybeStartSnapshotBuilder();

    return SnapshotParamsPromise_;
}

void TDecoratedAutomaton::ApplyMutations(const std::vector<TPendingMutationPtr>& mutations)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    for (const auto& mutation : mutations) {
        ApplyMutation(mutation);
    }
}

void TDecoratedAutomaton::ApplyMutation(const TPendingMutationPtr& mutation)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    TForbidContextSwitchGuard contextSwitchGuard;

    TMutationContext mutationContext(
        AutomatonVersion_,
        mutation->Request,
        mutation->Timestamp,
        mutation->RandomSeed,
        mutation->PrevRandomSeed,
        mutation->SequenceNumber,
        StateHash_);

    auto commitPromise = mutation->LocalCommitPromise;
    {
        NTracing::TTraceContextGuard traceContextGuard(mutation->Request.TraceContext);
        DoApplyMutation(&mutationContext, mutation->Version, mutation->Term);
    }

    if (commitPromise) {
        YT_VERIFY(GetState() == EPeerState::Leading);
        commitPromise.TrySet(TMutationResponse{
            EMutationResponseOrigin::Commit,
            mutationContext.GetResponseData()
        });
    } else {
        YT_VERIFY(GetState() == EPeerState::Following);
    }

    MaybeStartSnapshotBuilder();
}

void TDecoratedAutomaton::DoApplyMutation(TMutationContext* mutationContext, TVersion mutationVersion, int term)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto automatonVersion = GetAutomatonVersion();

    // Cannot access the request after the handler has been invoked since the latter
    // could submit more mutations and cause #PendingMutations_ to be reallocated.
    // So we'd better make the needed copies right away.
    // Cf. YT-6908.
    const auto& request = mutationContext->Request();
    auto mutationId = request.MutationId;

    {
        TMutationContextGuard mutationContextGuard(mutationContext);
        Automaton_->ApplyMutation(mutationContext);
    }

    mutationContext->CombineStateHash(mutationContext->GetRandomSeed());
    StateHash_ = mutationContext->GetStateHash();

    Timestamp_ = mutationContext->GetTimestamp();

    if (Options_.ResponseKeeper &&
        mutationId &&
        !mutationContext->GetResponseKeeperSuppressed() &&
        mutationContext->GetResponseData()) // Null when mutation idempotizer kicks in.
    {
        Options_.ResponseKeeper->EndRequest(mutationId, mutationContext->GetResponseData());
    }

    ++SequenceNumber_;
    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Applying mutation (SequenceNumber: %v, RandomSeed: %llx, Version: %v)",
        SequenceNumber_.load(),
        mutationContext->GetRandomSeed(),
        mutationVersion);

    // COMPAT(aleksandra-zh)
    YT_LOG_FATAL_IF(
        SequenceNumber_ != mutationContext->GetSequenceNumber() && mutationContext->GetSequenceNumber() != 0,
        "Sequence numbers differ (AutomatonSequenceNumber: %v, MutationSequenceNumber: %v)",
        SequenceNumber_.load(),
        mutationContext->GetSequenceNumber());

    // COMPAT(aleksandra-zh)
    YT_LOG_FATAL_IF(
        RandomSeed_ != mutationContext->GetPrevRandomSeed() && mutationContext->GetPrevRandomSeed() != 0,
        "Mutation random seeds differ (AutomatonRandomSeed: %llx, MutationRandomSeed: %llx)",
        RandomSeed_.load(),
        mutationContext->GetPrevRandomSeed());
    RandomSeed_ = mutationContext->GetRandomSeed();

    if (mutationVersion.SegmentId == automatonVersion.SegmentId) {
        YT_VERIFY(mutationVersion.RecordId == automatonVersion.RecordId);
    } else {
        YT_VERIFY(mutationVersion.SegmentId > automatonVersion.SegmentId);
        YT_VERIFY(mutationVersion.RecordId == 0);
    }
    AutomatonVersion_ = mutationVersion.Advance();

    LastMutationTerm_ = term;

    if (Config_->EnableStateHashChecker) {
        StateHashChecker_->Report(SequenceNumber_.load(), StateHash_);
    }
}

EPeerState TDecoratedAutomaton::GetState() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return State_;
}

TEpochContextPtr TDecoratedAutomaton::GetEpochContext()
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(EpochContextLock_);
    return EpochContext_;
}

ui64 TDecoratedAutomaton::GetStateHash() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return StateHash_.load();
}

i64 TDecoratedAutomaton::GetSequenceNumber() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SequenceNumber_.load();
}

i64 TDecoratedAutomaton::GetRandomSeed() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RandomSeed_.load();
}

int TDecoratedAutomaton::GetLastMutationTerm() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return LastMutationTerm_.load();
}

TReachableState TDecoratedAutomaton::GetReachableState() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return {AutomatonVersion_.load().SegmentId, SequenceNumber_.load()};
}

TInstant TDecoratedAutomaton::GetSnapshotBuildDeadline() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return SnapshotBuildDeadline_;
}

TVersion TDecoratedAutomaton::GetAutomatonVersion() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return AutomatonVersion_.load();
}

bool TDecoratedAutomaton::TryAcquireUserLock()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (SystemLock_.load() != 0) {
        return false;
    }
    ++UserLock_;
    if (SystemLock_.load() != 0) {
        --UserLock_;
        return false;
    }
    return true;
}

void TDecoratedAutomaton::ReleaseUserLock()
{
    VERIFY_THREAD_AFFINITY_ANY();

    --UserLock_;
}

void TDecoratedAutomaton::AcquireSystemLock()
{
    VERIFY_THREAD_AFFINITY_ANY();

    int result = ++SystemLock_;
    while (UserLock_.load() != 0) {
        SpinLockPause();
    }
    YT_LOG_DEBUG("System lock acquired (Lock: %v)",
        result);
}

void TDecoratedAutomaton::ReleaseSystemLock()
{
    VERIFY_THREAD_AFFINITY_ANY();

    int result = --SystemLock_;
    YT_LOG_DEBUG("System lock released (Lock: %v)",
        result);
}

void TDecoratedAutomaton::StartEpoch(TEpochContextPtr epochContext)
{
    auto guard = WriterGuard(EpochContextLock_);
    YT_VERIFY(!EpochContext_);
    std::swap(epochContext, EpochContext_);
}

void TDecoratedAutomaton::CancelSnapshot(const TError& error)
{
    if (SnapshotParamsPromise_ && SnapshotParamsPromise_.ToFuture().Cancel(error)) {
        YT_LOG_INFO(error, "Snapshot canceled");
    }
    SnapshotParamsPromise_.Reset();
}

void TDecoratedAutomaton::StopEpoch()
{
    EpochContext_.Reset();
}

void TDecoratedAutomaton::UpdateLastSuccessfulSnapshotInfo(const TErrorOr<TRemoteSnapshotParams>& snapshotInfoOrError)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (!snapshotInfoOrError.IsOK()) {
        return;
    }

    auto snapshotId = snapshotInfoOrError.Value().SnapshotId;
    LastSuccessfulSnapshotId_ = std::max(LastSuccessfulSnapshotId_.load(), snapshotId);
}

void TDecoratedAutomaton::UpdateSnapshotBuildDeadline()
{
    SnapshotBuildDeadline_ =
        TInstant::Now() +
        Config_->SnapshotBuildPeriod +
        RandomDuration(Config_->SnapshotBuildSplay);
}

void TDecoratedAutomaton::MaybeStartSnapshotBuilder()
{
    if (GetSequenceNumber() != SnapshotSequenceNumber_) {
        return;
    }

    auto builder =
        // XXX(babenko): ASAN + fork = possible deadlock; cf. https://st.yandex-team.ru/DEVTOOLS-5425
#ifdef _asan_enabled_
        false
#else
        Options_.UseFork
#endif
        ? TIntrusivePtr<TSnapshotBuilderBase>(New<TForkSnapshotBuilder>(this, ForkCounters_))
        : TIntrusivePtr<TSnapshotBuilderBase>(New<TNoForkSnapshotBuilder>(this));

    auto buildResult = builder->Run();
    buildResult.Subscribe(
        BIND(&TDecoratedAutomaton::UpdateLastSuccessfulSnapshotInfo, MakeWeak(this))
        .Via(AutomatonInvoker_));

    SnapshotParamsPromise_.SetFrom(buildResult);
}

bool TDecoratedAutomaton::IsRecovery() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return
        State_ == EPeerState::LeaderRecovery ||
        State_ == EPeerState::FollowerRecovery;
}

bool TDecoratedAutomaton::IsMutationLoggingEnabled() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return !IsRecovery() || Config_->ForceMutationLogging;
}

bool TDecoratedAutomaton::IsBuildingSnapshotNow() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return BuildingSnapshot_.load();
}

int TDecoratedAutomaton::GetLastSuccessfulSnapshotId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return LastSuccessfulSnapshotId_.load();
}

TReign TDecoratedAutomaton::GetCurrentReign() const
{
    return Automaton_->GetCurrentReign();
}

EFinalRecoveryAction TDecoratedAutomaton::GetFinalRecoveryAction() const
{
    return Automaton_->GetFinalRecoveryAction();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra2
