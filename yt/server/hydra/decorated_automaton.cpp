#include "decorated_automaton.h"
#include "automaton.h"
#include "changelog.h"
#include "config.h"
#include "mutation_context.h"
#include "serialize.h"
#include "snapshot.h"
#include "snapshot_discovery.h"

#include <yt/server/misc/fork_snapshot_builder.h>

#include <yt/ytlib/election/cell_manager.h>

#include <yt/ytlib/hydra/hydra_manager.pb.h>
#include <yt/ytlib/hydra/hydra_service.pb.h>

#include <yt/core/actions/invoker_detail.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/blob.h>
#include <yt/core/misc/common.h>
#include <yt/core/misc/proc.h>

#include <yt/core/pipes/async_reader.h>
#include <yt/core/pipes/pipe.h>

#include <yt/core/profiling/scoped_timer.h>

#include <util/random/random.h>

#include <util/system/file.h>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;
using namespace NElection;
using namespace NRpc;
using namespace NHydra::NProto;
using namespace NPipes;

////////////////////////////////////////////////////////////////////////////////

static const i64 SnapshotTransferBlockSize = (i64) 1024 * 1024;
static const auto& Profiler = HydraProfiler;

////////////////////////////////////////////////////////////////////////////////

TSystemLockGuard::TSystemLockGuard()
{ }

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

TUserLockGuard::TUserLockGuard()
{ }

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

    virtual void Invoke(const TClosure& callback) override
    {
        auto lockGuard = TSystemLockGuard::Acquire(Owner_);

        auto doInvoke = [=, this_ = MakeStrong(this)] (TSystemLockGuard /*lockGuard*/) {
            TCurrentInvokerGuard currentInvokerGuard(this_);
            callback.Run();
        };

        Owner_->AutomatonInvoker_->Invoke(BIND(doInvoke, Passed(std::move(lockGuard))));
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

    virtual void Invoke(const TClosure& callback) override
    {
        auto lockGuard = TUserLockGuard::TryAcquire(Owner_);
        if (!lockGuard)
            return;

        auto doInvoke = [=, this_ = MakeStrong(this)] () {
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
    : public virtual TRefCounted
{
public:
    TSnapshotBuilderBase(
        TDecoratedAutomatonPtr owner,
        TVersion snapshotVersion)
        : Owner_(owner)
          , SnapshotVersion_(snapshotVersion)
          , SnapshotId_(SnapshotVersion_.SegmentId + 1)
    { }

    ~TSnapshotBuilderBase()
    {
        ReleaseLock();
    }

    TFuture<TRemoteSnapshotParams> Run()
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        auto* logger = GetLogger();
        *logger = Owner_->Logger;
        logger->AddTag("SnapshotId: %v", SnapshotId_);

        try {
            TryAcquireLock();

            TSnapshotMeta meta;
            meta.set_prev_record_count(SnapshotVersion_.RecordId);

            SnapshotWriter_ = Owner_->SnapshotStore_->CreateWriter(SnapshotId_, meta);

            return DoRun().Apply(
                BIND(&TSnapshotBuilderBase::OnFinished, MakeStrong(this))
                    .AsyncVia(GetHydraIOInvoker()));
        } catch (const std::exception& ex) {
            ReleaseLock();
            return MakeFuture<TRemoteSnapshotParams>(TError(ex));
        }
    }

protected:
    const TDecoratedAutomatonPtr Owner_;
    const TVersion SnapshotVersion_;
    const int SnapshotId_;

    ISnapshotWriterPtr SnapshotWriter_;


    virtual TFuture<void> DoRun() = 0;
    virtual NLogging::TLogger* GetLogger() = 0;

    void TryAcquireLock()
    {
        if (Owner_->BuildingSnapshot_.test_and_set()) {
            THROW_ERROR_EXCEPTION("Cannot start building snapshot %v since another snapshot is still being constructed",
                SnapshotId_);
        }
        LockAcquired_ = true;
    }

    void ReleaseLock()
    {
        if (LockAcquired_) {
            Owner_->BuildingSnapshot_.clear();
            LockAcquired_ = false;
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
        remoteParams.PeerId = Owner_->CellManager_->GetSelfPeerId();
        remoteParams.SnapshotId = SnapshotId_;
        static_cast<TSnapshotParams&>(remoteParams) = params;
        return remoteParams;
    }

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TForkSnapshotBuilder
    : public TSnapshotBuilderBase
    , public TForkSnapshotBuilderBase
{
public:
    TForkSnapshotBuilder(
        TDecoratedAutomatonPtr owner,
        TVersion snapshotVersion)
        : TDecoratedAutomaton::TSnapshotBuilderBase(owner, snapshotVersion)
    { }

private:
    TAsyncReaderPtr InputStream_;
    std::unique_ptr<TFile> OutputFile_;

    TFuture<void> AsyncTransferResult_;


    virtual TFuture<void> DoRun() override
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        auto pipe = TPipeFactory().Create();
        LOG_INFO("Snapshot transfer pipe opened (Pipe: {%v})",
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

    virtual NLogging::TLogger* GetLogger() override
    {
        return &Logger;
    }

    virtual TDuration GetTimeout() const override
    {
        return Owner_->Config_->SnapshotBuildTimeout;
    }

    virtual void RunChild() override
    {
        CloseAllDescriptors({
            2, // stderr
            int(OutputFile_->GetHandle())
        });
        TFileOutput output(*OutputFile_);
        auto writer = CreateAsyncAdapter(&output);
        Owner_->SaveSnapshot(writer)
            .Get()
            .ThrowOnError();
        OutputFile_->Close();
    }

    virtual void RunParent() override
    {
        OutputFile_->Close();
    }

    virtual void Cleanup() override
    {
        ReleaseLock();
    }

    void TransferLoop()
    {
        LOG_INFO("Snapshot transfer loop started");

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

        LOG_INFO("Snapshot transfer loop completed (Size: %v)",
            size);
    }

    void OnFinished()
    {
        WaitFor(AsyncTransferResult_)
            .ThrowOnError();

        WaitFor(SnapshotWriter_->Close())
            .ThrowOnError();
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
        TGuard<TSpinLock> guard(SpinLock_);
        SuspendedPromise_ = NewPromise<void>();
    }

    void ResumeAsAsync(IAsyncOutputStreamPtr underlyingStream)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto suspendedPromise = SuspendedPromise_;
        SuspendedPromise_.Reset();
        UnderlyingStream_ = CreateZeroCopyAdapter(underlyingStream);
        for (const auto& syncBlock : SyncBlocks_) {
            ForwardBlock(syncBlock);
        }
        guard.Release();
        suspendedPromise.Set();
    }

    TFuture<void> Finish()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return LastForwardResult_;
    }

    virtual TFuture<void> Write(const TSharedRef& block) override
    {
        struct TBlockTag { };
        auto blockCopy = TSharedRef::MakeCopy<TBlockTag>(block);

        TGuard<TSpinLock> guard(SpinLock_);
        if (UnderlyingStream_) {
            LOG_TRACE("Got async snapshot block (Size: %v)", blockCopy.Size());
            AsyncSize_ += block.Size();
            return ForwardBlock(blockCopy);
        } else {
            LOG_TRACE("Got sync snapshot block (Size: %v)", blockCopy.Size());
            SyncBlocks_.push_back(blockCopy);
            SyncSize_ += block.Size();
            return SuspendedPromise_ ? SuspendedPromise_.ToFuture() : VoidFuture;
        }
    }

    i64 GetSyncSize() const
    {
        YCHECK(UnderlyingStream_);
        return SyncSize_;
    }

    i64 GetAsyncSize() const
    {
        YCHECK(UnderlyingStream_);
        return AsyncSize_;
    }

private:
    const NLogging::TLogger Logger;

    TSpinLock SpinLock_;
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
    TNoForkSnapshotBuilder(
        TDecoratedAutomatonPtr owner,
        TVersion snapshotVersion)
        : TDecoratedAutomaton::TSnapshotBuilderBase(owner, snapshotVersion)
    { }

private:
    NLogging::TLogger Logger;

    TPromise<void> RunPromise_ = NewPromise<void>();
    TIntrusivePtr<TSwitchableSnapshotWriter> SwitchableSnapshotWriter_;

    TFuture<void> AsyncSnapshotResult_;


    virtual TFuture<void> DoRun() override
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        SwitchableSnapshotWriter_ = New<TSwitchableSnapshotWriter>(Logger);

        auto asyncOpenResult = SnapshotWriter_->Open();

        LOG_INFO("Snapshot sync phase started");

        AsyncSnapshotResult_ = Owner_->SaveSnapshot(SwitchableSnapshotWriter_);

        LOG_INFO("Snapshot sync phase completed");

        SwitchableSnapshotWriter_->Suspend();

        // NB: Only switch to async writer when the sync phase is complete.
        asyncOpenResult.Subscribe(
            BIND(&TNoForkSnapshotBuilder::OnWriterOpened, MakeStrong(this))
                .Via(GetHydraIOInvoker()));

        return RunPromise_;
    }

    virtual NLogging::TLogger* GetLogger() override
    {
        return &Logger;
    }

    void OnWriterOpened(const TError& error)
    {
        try {
            error.ThrowOnError();

            LOG_INFO("Switching to async snapshot writer");

            SwitchableSnapshotWriter_->ResumeAsAsync(SnapshotWriter_);

            AsyncSnapshotResult_.Subscribe(
                BIND(&TNoForkSnapshotBuilder::OnSnapshotSaved, MakeStrong(this))
                    .Via(GetHydraIOInvoker()));
        } catch (const std::exception& ex) {
            RunPromise_.TrySet(TError(ex));
        }
    }

    void OnSnapshotSaved(const TError& error)
    {
        try {
            error.ThrowOnError();

            LOG_INFO("Snapshot async phase completed (SyncSize: %v, AsyncSize: %v)",
                SwitchableSnapshotWriter_->GetSyncSize(),
                SwitchableSnapshotWriter_->GetAsyncSize());

            WaitFor(SwitchableSnapshotWriter_->Finish())
                .ThrowOnError();

            WaitFor(SnapshotWriter_->Close())
                .ThrowOnError();

            RunPromise_.TrySet();
        } catch (const std::exception& ex) {
            RunPromise_.TrySet(TError(ex));
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

TDecoratedAutomaton::TDecoratedAutomaton(
    TDistributedHydraManagerConfigPtr config,
    TCellManagerPtr cellManager,
    IAutomatonPtr automaton,
    IInvokerPtr automatonInvoker,
    IInvokerPtr controlInvoker,
    ISnapshotStorePtr snapshotStore,
    const TDistributedHydraManagerOptions& options)
    : State_(EPeerState::Stopped)
    , Config_(config)
    , CellManager_(cellManager)
    , Automaton_(automaton)
    , AutomatonInvoker_(automatonInvoker)
    , DefaultGuardedUserInvoker_(CreateGuardedUserInvoker(AutomatonInvoker_))
    , ControlInvoker_(controlInvoker)
    , SystemInvoker_(New<TSystemInvoker>(this))
    , SnapshotStore_(snapshotStore)
    , Options_(options)
    , BatchCommitTimeCounter_("/batch_commit_time")
    , Logger(HydraLogger)
{
    YCHECK(Config_);
    YCHECK(CellManager_);
    YCHECK(Automaton_);
    YCHECK(ControlInvoker_);
    YCHECK(SnapshotStore_);

    VERIFY_INVOKER_THREAD_AFFINITY(AutomatonInvoker_, AutomatonThread);
    VERIFY_INVOKER_THREAD_AFFINITY(ControlInvoker_, ControlThread);

    Logger.AddTag("CellId: %v", CellManager_->GetCellId());

    BuildingSnapshot_.clear();
    AutomatonVersion_ = TVersion();
    StopEpoch();
}

void TDecoratedAutomaton::Initialize()
{
    AutomatonInvoker_->Invoke(BIND(&IAutomaton::Clear, Automaton_));
}

void TDecoratedAutomaton::OnStartLeading(TEpochContextPtr epochContext)
{
    YCHECK(State_ == EPeerState::Stopped);
    State_ = EPeerState::LeaderRecovery;
    StartEpoch(epochContext);
}

void TDecoratedAutomaton::OnLeaderRecoveryComplete()
{
    YCHECK(State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Leading;
    LastSnapshotTime_ = TInstant::Now();
}

void TDecoratedAutomaton::OnStopLeading()
{
    YCHECK(State_ == EPeerState::Leading || State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Stopped;
    StopEpoch();
}

void TDecoratedAutomaton::OnStartFollowing(TEpochContextPtr epochContext)
{
    YCHECK(State_ == EPeerState::Stopped);
    State_ = EPeerState::FollowerRecovery;
    StartEpoch(epochContext);
}

void TDecoratedAutomaton::OnFollowerRecoveryComplete()
{
    YCHECK(State_ == EPeerState::FollowerRecovery);
    State_ = EPeerState::Following;
    LastSnapshotTime_ = TInstant::Now();
}

void TDecoratedAutomaton::OnStopFollowing()
{
    YCHECK(State_ == EPeerState::Following || State_ == EPeerState::FollowerRecovery);
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
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TContextSwitchedGuard guard(BIND([] () {
        // Context switches are not allowed during sync phase.
        YUNREACHABLE();
    }));

    return Automaton_->SaveSnapshot(writer);
}

void TDecoratedAutomaton::LoadSnapshot(TVersion version, IAsyncZeroCopyInputStreamPtr reader)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LOG_INFO("Started loading snapshot %v to reach version %v",
        version.SegmentId + 1,
        version);

    Changelog_.Reset();

    PROFILE_TIMING ("/snapshot_load_time") {
        Automaton_->Clear();
        try {
            Automaton_->LoadSnapshot(reader);
        } catch (...) {
            // Don't leave the state corrupted.
            Automaton_->Clear();
            throw;
        }
    }

    LOG_INFO("Finished loading snapshot");

    AutomatonVersion_ = version;
}

void TDecoratedAutomaton::ApplyMutationDuringRecovery(const TSharedRef& recordData)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TMutationHeader header;
    TSharedRef requestData;
    DeserializeMutationRecord(recordData, &header, &requestData);

    auto mutationVersion = TVersion(header.segment_id(), header.record_id());
    RotateAutomatonVersionIfNeeded(mutationVersion);

    TMutationRequest request(header.mutation_type(), requestData);

    TMutationContext context(
        AutomatonVersion_,
        request,
        TInstant(header.timestamp()),
        header.random_seed());

    DoApplyMutation(&context);
}

void TDecoratedAutomaton::LogLeaderMutation(
    const TMutationRequest& request,
    TSharedRef* recordData,
    TFuture<void>* localFlushResult,
    TFuture<TMutationResponse>* commitResult)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(recordData);
    YASSERT(localFlushResult);
    YASSERT(commitResult);

    TPendingMutation pendingMutation;
    pendingMutation.Version = LoggedVersion_;
    pendingMutation.Request = request;
    pendingMutation.Timestamp = TInstant::Now();
    pendingMutation.RandomSeed  = RandomNumber<ui64>();
    pendingMutation.CommitPromise = NewPromise<TMutationResponse>();
    PendingMutations_.push(pendingMutation);

    MutationHeader_.Clear(); // don't forget to cleanup the pooled instance
    MutationHeader_.set_mutation_type(request.Type);
    MutationHeader_.set_timestamp(pendingMutation.Timestamp.GetValue());
    MutationHeader_.set_random_seed(pendingMutation.RandomSeed);
    MutationHeader_.set_segment_id(pendingMutation.Version.SegmentId);
    MutationHeader_.set_record_id(pendingMutation.Version.RecordId);

    *recordData = SerializeMutationRecord(MutationHeader_, request.Data);
    *localFlushResult = Changelog_->Append(*recordData);
    *commitResult = pendingMutation.CommitPromise;

    LoggedVersion_ = pendingMutation.Version.Advance();
}

void TDecoratedAutomaton::LogFollowerMutation(
    const TSharedRef& recordData,
    TFuture<void>* logResult)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TSharedRef mutationData;
    DeserializeMutationRecord(recordData, &MutationHeader_, &mutationData);

    TPendingMutation pendingMutation;
    pendingMutation.Version = LoggedVersion_;
    pendingMutation.Request.Type = MutationHeader_.mutation_type();
    pendingMutation.Request.Data = mutationData;
    pendingMutation.Timestamp = TInstant(MutationHeader_.timestamp());
    pendingMutation.RandomSeed  = MutationHeader_.random_seed();
    PendingMutations_.push(pendingMutation);

    auto actualLogResult = Changelog_->Append(recordData);
    if (logResult) {
        *logResult = std::move(actualLogResult);
    }

    LoggedVersion_ = pendingMutation.Version.Advance();
}

TFuture<TRemoteSnapshotParams> TDecoratedAutomaton::BuildSnapshot()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto loggedVersion = GetLoggedVersion();

    LOG_INFO("Scheduled snapshot at version %v",
        loggedVersion);

    LastSnapshotTime_ = TInstant::Now();
    SnapshotVersion_ = loggedVersion;

    if (SnapshotParamsPromise_) {
        SnapshotParamsPromise_.ToFuture().Cancel();
    }
    SnapshotParamsPromise_ = NewPromise<TRemoteSnapshotParams>();

    MaybeStartSnapshotBuilder();

    return SnapshotParamsPromise_;
}

TFuture<void> TDecoratedAutomaton::RotateChangelog()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto loggedVersion = GetLoggedVersion();

    LOG_INFO("Rotating changelog at version %v",
        loggedVersion);

    return BIND(&TDecoratedAutomaton::DoRotateChangelog, MakeStrong(this))
        .AsyncVia(EpochContext_->EpochUserAutomatonInvoker)
        .Run();
}

void TDecoratedAutomaton::DoRotateChangelog()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    WaitFor(Changelog_->Flush())
        .ThrowOnError();

    TChangelogMeta meta;
    meta.set_prev_record_count(Changelog_->GetRecordCount());

    auto loggedVersion = GetLoggedVersion();
    auto asyncNewChangelog = EpochContext_->ChangelogStore->CreateChangelog(
        loggedVersion.SegmentId + 1,
        meta);
    Changelog_ = WaitFor(asyncNewChangelog)
        .ValueOrThrow();
    LoggedVersion_ = loggedVersion.Rotate();

    LOG_INFO("Changelog rotated");
}

void TDecoratedAutomaton::CommitMutations(TVersion version, bool mayYield)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (version <= CommittedVersion_)
        return;

    CommittedVersion_ = version;

    LOG_DEBUG("Committed version promoted to %v",
        version);

    ApplyPendingMutations(mayYield);
}

bool TDecoratedAutomaton::HasReadyMutations() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (PendingMutations_.empty()) {
        return false;
    }

    const auto& pendingMutation = PendingMutations_.front();
    return pendingMutation.Version < CommittedVersion_;
}

void TDecoratedAutomaton::ApplyPendingMutations(bool mayYield)
{
    NProfiling::TScopedTimer timer;
    PROFILE_AGGREGATED_TIMING (BatchCommitTimeCounter_) {
        while (!PendingMutations_.empty()) {
            auto& pendingMutation = PendingMutations_.front();
            if (pendingMutation.Version >= CommittedVersion_)
                break;

            RotateAutomatonVersionIfNeeded(pendingMutation.Version);

            TMutationContext context(
                AutomatonVersion_,
                pendingMutation.Request,
                pendingMutation.Timestamp,
                pendingMutation.RandomSeed);

            DoApplyMutation(&context);

            if (pendingMutation.CommitPromise) {
                pendingMutation.CommitPromise.Set(context.Response());
            }

            PendingMutations_.pop();

            MaybeStartSnapshotBuilder();

            if (mayYield && timer.GetElapsed() > Config_->MaxCommitBatchDuration) {
                EpochContext_->EpochUserAutomatonInvoker->Invoke(
                    BIND(&TDecoratedAutomaton::ApplyPendingMutations, MakeStrong(this), true));
                break;
            }
        }
    }
}

void TDecoratedAutomaton::RotateAutomatonVersionIfNeeded(TVersion mutationVersion)
{
    auto automatonVersion = GetAutomatonVersion();
    if (mutationVersion.SegmentId == automatonVersion.SegmentId) {
        YCHECK(mutationVersion.RecordId == automatonVersion.RecordId);
    } else {
        YCHECK(mutationVersion.SegmentId > automatonVersion.SegmentId);
        YCHECK(mutationVersion.RecordId == 0);
        RotateAutomatonVersion(mutationVersion.SegmentId);
    }
}

void TDecoratedAutomaton::DoApplyMutation(TMutationContext* context)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    const auto& request = context->Request();
    auto automatonVersion = GetAutomatonVersion();

    LOG_DEBUG_UNLESS(IsRecovery(), "Applying mutation (Version: %v, MutationType: %v)",
        automatonVersion,
        request.Type);

    TMutationContextGuard contextGuard(context);

    if (request.Action) {
        request.Action.Run(context);
    } else {
        Automaton_->ApplyMutation(context);
    }

    AutomatonVersion_ = automatonVersion.Advance();
}

TVersion TDecoratedAutomaton::GetLoggedVersion() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return LoggedVersion_;
}

void TDecoratedAutomaton::SetChangelog(IChangelogPtr changelog)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Changelog_ = changelog;
}

void TDecoratedAutomaton::SetLoggedVersion(TVersion version)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LoggedVersion_ = version;
}

i64 TDecoratedAutomaton::GetLoggedDataSize() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return Changelog_->GetDataSize();
}

TInstant TDecoratedAutomaton::GetLastSnapshotTime() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return LastSnapshotTime_;
}

TVersion TDecoratedAutomaton::GetAutomatonVersion() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return AutomatonVersion_;
}

void TDecoratedAutomaton::RotateAutomatonVersion(int segmentId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto automatonVersion = GetAutomatonVersion();
    YCHECK(automatonVersion.SegmentId < segmentId);
    automatonVersion = TVersion(segmentId, 0);
    AutomatonVersion_ = automatonVersion;

    LOG_INFO("Automaton version is rotated to %v",
        automatonVersion);
}

bool TDecoratedAutomaton::TryAcquireUserLock()
{
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
    --UserLock_;
}

void TDecoratedAutomaton::AcquireSystemLock()
{
    int result = ++SystemLock_;
    while (UserLock_.load() != 0) {
        SpinLockPause();
    }
    LOG_DEBUG("System lock acquired (Lock: %v)",
        result);
}

void TDecoratedAutomaton::ReleaseSystemLock()
{
    int result = --SystemLock_;
    LOG_DEBUG("System lock released (Lock: %v)",
        result);
}

void TDecoratedAutomaton::StartEpoch(TEpochContextPtr epochContext)
{
    YCHECK(!EpochContext_);
    EpochContext_ = epochContext;
    LoggedVersion_ = epochContext->ChangelogStore->GetReachableVersion();
}

void TDecoratedAutomaton::StopEpoch()
{
    auto error = TError(NHydra::EErrorCode::MaybeCommitted, "Peer stopped");
    while (!PendingMutations_.empty()) {
        auto& pendingMutation = PendingMutations_.front();
        if (pendingMutation.CommitPromise) {
            pendingMutation.CommitPromise.Set(error);
        }
        PendingMutations_.pop();
    }

    Changelog_.Reset();
    EpochContext_.Reset();
    SnapshotVersion_ = TVersion();
    LoggedVersion_ = TVersion();
    CommittedVersion_ = TVersion();
    if (SnapshotParamsPromise_) {
        SnapshotParamsPromise_.ToFuture().Cancel();
        SnapshotParamsPromise_.Reset();
    }
}

void TDecoratedAutomaton::MaybeStartSnapshotBuilder()
{
    if (GetAutomatonVersion() != SnapshotVersion_)
        return;

    auto builder = Options_.UseFork
       ? TIntrusivePtr<TSnapshotBuilderBase>(New<TForkSnapshotBuilder>(this, SnapshotVersion_))
       : TIntrusivePtr<TSnapshotBuilderBase>(New<TNoForkSnapshotBuilder>(this, SnapshotVersion_));
    SnapshotParamsPromise_.SetFrom(builder->Run());
}

bool TDecoratedAutomaton::IsRecovery()
{
    return
        State_ == EPeerState::LeaderRecovery ||
        State_ == EPeerState::FollowerRecovery;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
