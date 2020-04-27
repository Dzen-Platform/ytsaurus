#include "composite_automaton.h"
#include "private.h"
#include "hydra_manager.h"
#include "mutation_context.h"
#include "snapshot.h"

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/serialize.h>

#include <util/stream/buffered.h>

namespace NYT::NHydra {

using namespace NConcurrency;
using namespace NProfiling;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

static const size_t SnapshotLoadBufferSize = 64_KB;
static const size_t SnapshotSaveBufferSize = 64_KB;
static const size_t SnapshotPrefetchWindowSize = 64_MB;

////////////////////////////////////////////////////////////////////////////////

TCompositeAutomatonPart::TCompositeAutomatonPart(
    IHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton,
    IInvokerPtr automatonInvoker)
    : HydraManager_(hydraManager.Get())
    , Automaton_(automaton.Get())
    , AutomatonInvoker_(std::move(automatonInvoker))
{
    YT_VERIFY(HydraManager_);
    YT_VERIFY(Automaton_);

    HydraManager_->SubscribeStartLeading(BIND(&TThis::OnStartLeading, MakeWeak(this)));
    HydraManager_->SubscribeStartLeading(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
    HydraManager_->SubscribeLeaderRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));
    HydraManager_->SubscribeLeaderRecoveryComplete(BIND(&TThis::OnLeaderRecoveryComplete, MakeWeak(this)));
    HydraManager_->SubscribeLeaderActive(BIND(&TThis::OnLeaderActive, MakeWeak(this)));
    HydraManager_->SubscribeStopLeading(BIND(&TThis::OnStopLeading, MakeWeak(this)));

    HydraManager_->SubscribeStartFollowing(BIND(&TThis::OnStartFollowing, MakeWeak(this)));
    HydraManager_->SubscribeStartFollowing(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
    HydraManager_->SubscribeFollowerRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));
    HydraManager_->SubscribeFollowerRecoveryComplete(BIND(&TThis::OnFollowerRecoveryComplete, MakeWeak(this)));
    HydraManager_->SubscribeStopFollowing(BIND(&TThis::OnStopFollowing, MakeWeak(this)));

    Automaton_->RegisterPart(this);
}

void TCompositeAutomatonPart::RegisterSaver(
    ESyncSerializationPriority priority,
    const TString& name,
    TCallback<void(TSaveContext&)> callback)
{
    // Check for duplicate part names.
    YT_VERIFY(Automaton_->SaverPartNames_.insert(name).second);

    TCompositeAutomaton::TSyncSaverDescriptor descriptor;
    descriptor.Priority = priority;
    descriptor.Name = name;
    descriptor.Callback = callback;
    descriptor.SnapshotVersion = GetCurrentSnapshotVersion();
    Automaton_->SyncSavers_.push_back(descriptor);
}

void TCompositeAutomatonPart::RegisterSaver(
    EAsyncSerializationPriority priority,
    const TString& name,
    TCallback<TCallback<void(TSaveContext&)>()> callback)
{
    // Check for duplicate part names.
    YT_VERIFY(Automaton_->SaverPartNames_.insert(name).second);

    TCompositeAutomaton::TAsyncSaverDescriptor descriptor;
    descriptor.Priority = priority;
    descriptor.Name = name;
    descriptor.Callback = callback;
    descriptor.SnapshotVersion = GetCurrentSnapshotVersion();
    Automaton_->AsyncSavers_.push_back(descriptor);
}

void TCompositeAutomatonPart::RegisterLoader(
    const TString& name,
    TCallback<void(TLoadContext&)> callback)
{
    TCompositeAutomaton::TLoaderDescriptor descriptor;
    descriptor.Name = name;
    descriptor.Callback = BIND([=] (TLoadContext& context) {
        if (!ValidateSnapshotVersion(context.GetVersion())) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidSnapshotVersion,
                "Unsupported snapshot version %v in part %v",
                context.GetVersion(),
                name);
        }
        callback.Run(context);
    });
    YT_VERIFY(Automaton_->PartNameToLoaderDescriptor_.insert(std::make_pair(name, descriptor)).second);
}

void TCompositeAutomatonPart::RegisterMethod(
    const TString& type,
    TCallback<void(TMutationContext*)> callback)
{
    TTagIdList tagIds{
        TProfileManager::Get()->RegisterTag("type", type)
    };
    TCompositeAutomaton::TMethodDescriptor descriptor{
        callback,
        TMonotonicCounter("/cumulative_mutation_time", tagIds)
    };
    YT_VERIFY(Automaton_->MethodNameToDescriptor_.insert(std::make_pair(type, descriptor)).second);
}

bool TCompositeAutomatonPart::ValidateSnapshotVersion(int /*version*/)
{
    return true;
}

int TCompositeAutomatonPart::GetCurrentSnapshotVersion()
{
    return 0;
}

void TCompositeAutomatonPart::Clear()
{ }

void TCompositeAutomatonPart::SetZeroState()
{ }

void TCompositeAutomatonPart::OnBeforeSnapshotLoaded()
{ }

void TCompositeAutomatonPart::OnAfterSnapshotLoaded()
{ }

bool TCompositeAutomatonPart::IsLeader() const
{
    return HydraManager_->IsLeader();
}

bool TCompositeAutomatonPart::IsFollower() const
{
    return HydraManager_->IsFollower();
}

bool TCompositeAutomatonPart::IsRecovery() const
{
    return HydraManager_->IsRecovery();
}

void TCompositeAutomatonPart::OnStartLeading()
{
    StartEpoch();
}

void TCompositeAutomatonPart::OnLeaderRecoveryComplete()
{ }

void TCompositeAutomatonPart::OnLeaderActive()
{ }

void TCompositeAutomatonPart::OnStopLeading()
{
    StopEpoch();
}

void TCompositeAutomatonPart::OnStartFollowing()
{
    StartEpoch();
}

void TCompositeAutomatonPart::OnFollowerRecoveryComplete()
{ }

void TCompositeAutomatonPart::OnStopFollowing()
{
    StopEpoch();
}

void TCompositeAutomatonPart::OnRecoveryStarted()
{ }

void TCompositeAutomatonPart::OnRecoveryComplete()
{ }

void TCompositeAutomatonPart::StartEpoch()
{
    EpochAutomatonInvoker_ = HydraManager_
        ->GetAutomatonCancelableContext()
        ->CreateInvoker(AutomatonInvoker_);
    Automaton_->FinalRecoveryAction_ = EFinalRecoveryAction::None;
}

void TCompositeAutomatonPart::StopEpoch()
{
    EpochAutomatonInvoker_.Reset();
}

void TCompositeAutomatonPart::LogHandlerError(const TError& error)
{
    if (!IsRecovery()) {
        Automaton_->LogHandlerError(error);
    }
}

////////////////////////////////////////////////////////////////////////////////

TCompositeAutomaton::TCompositeAutomaton(
    IInvokerPtr asyncSnapshotInvoker,
    TCellId cellId,
    const NProfiling::TTagIdList& profilingTagIds)
    : Logger(NLogging::TLogger(HydraLogger)
        .AddTag("CellId: %v", cellId))
    , Profiler(HydraProfiler.AddTags(profilingTagIds))
    , AsyncSnapshotInvoker_(asyncSnapshotInvoker)
{ }

void TCompositeAutomaton::SetSerializationDumpEnabled(bool value)
{
    SerializationDumpEnabled_ = value;
}

void TCompositeAutomaton::SetLowerWriteCountDumpLimit(i64 lowerLimit)
{
    LowerWriteCountDumpLimit_ = lowerLimit;
}

void TCompositeAutomaton::SetUpperWriteCountDumpLimit(i64 upperLimit)
{
    UpperWriteCountDumpLimit_ = upperLimit;
}

void TCompositeAutomaton::SetEnableTotalWriteCountReport(bool enableTotalWriteCountReport)
{
    EnableTotalWriteCountReport_ = enableTotalWriteCountReport;
}

void TCompositeAutomaton::RegisterPart(TCompositeAutomatonPartPtr part)
{
    YT_VERIFY(part);

    Parts_.push_back(part);

    if (HydraManager_) {
        YT_VERIFY(HydraManager_ == part->HydraManager_);
    } else {
        HydraManager_ = part->HydraManager_;

        HydraManager_->SubscribeStartLeading(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
        HydraManager_->SubscribeLeaderRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));

        HydraManager_->SubscribeStartFollowing(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
        HydraManager_->SubscribeFollowerRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));
    }
}

void TCompositeAutomaton::InitSaveContext(
    TSaveContext& context,
    ICheckpointableOutputStream* output)
{
    context.SetOutput(output);
    context.SetCheckpointableOutput(output);
}

void TCompositeAutomaton::InitLoadContext(
    TLoadContext& context,
    ICheckpointableInputStream* input)
{
    context.SetInput(input);
    context.SetCheckpointableInput(input);
    context.Dumper().SetEnabled(SerializationDumpEnabled_);
    context.Dumper().SetLowerWriteCountDumpLimit(LowerWriteCountDumpLimit_);
    context.Dumper().SetUpperWriteCountDumpLimit(UpperWriteCountDumpLimit_);
    context.SetEnableTotalWriteCountReport(EnableTotalWriteCountReport_);
}

TFuture<void> TCompositeAutomaton::SaveSnapshot(IAsyncOutputStreamPtr writer)
{
    DoSaveSnapshot(
        writer,
        // NB: Do not yield in sync part.
        ESyncStreamAdapterStrategy::Get,
        [&] (TSaveContext& context) {
            using NYT::Save;

            int partCount = SyncSavers_.size() + AsyncSavers_.size();
            Save<i32>(context, partCount);

            // Sort by (priority, name).
            auto syncSavers = SyncSavers_;
            std::sort(
                syncSavers.begin(),
                syncSavers.end(),
                [] (const TSyncSaverDescriptor& lhs, const TSyncSaverDescriptor& rhs) {
                    return
                        lhs.Priority < rhs.Priority ||
                        lhs.Priority == rhs.Priority && lhs.Name < rhs.Name;
                });

            for (const auto& descriptor : syncSavers) {
                WritePartHeader(context, descriptor);
                descriptor.Callback.Run(context);
            }
        });

    if (AsyncSavers_.empty()) {
        return VoidFuture;
    }

    YT_VERIFY(AsyncSnapshotInvoker_);

    std::vector<TCallback<void(TSaveContext&)>> asyncCallbacks;

    // Sort by (priority, name).
    auto asyncSavers = AsyncSavers_;
    std::sort(
        asyncSavers.begin(),
        asyncSavers.end(),
        [] (const TAsyncSaverDescriptor& lhs, const TAsyncSaverDescriptor& rhs) {
            return
                lhs.Priority < rhs.Priority ||
                lhs.Priority == rhs.Priority && lhs.Name < rhs.Name;
        });

    for (const auto& descriptor : asyncSavers) {
        asyncCallbacks.push_back(descriptor.Callback.Run());
    }

    // NB: Hold the parts strongly during the async phase.
    return
        BIND([=, this_ = MakeStrong(this), parts_ = GetParts()] () {
            DoSaveSnapshot(
                writer,
                // NB: Can yield in async part.
                ESyncStreamAdapterStrategy::WaitFor,
                [&] (TSaveContext& context) {
                    for (int index = 0; index < asyncSavers.size(); ++index) {
                        WritePartHeader(context, asyncSavers[index]);
                        asyncCallbacks[index].Run(context);
                    }
                });
        })
        .AsyncVia(AsyncSnapshotInvoker_)
        .Run();
}

void TCompositeAutomaton::LoadSnapshot(IAsyncZeroCopyInputStreamPtr reader)
{
    DoLoadSnapshot(
        reader,
        [&] (TLoadContext& context) {
            using NYT::Load;

            auto parts = GetParts();
            for (const auto& part : parts) {
                part->OnBeforeSnapshotLoaded();
            }

            int partCount = LoadSuspended<i32>(context);
            SERIALIZATION_DUMP_WRITE(context, "parts[%v]", partCount);
            SERIALIZATION_DUMP_INDENT(context) {
                for (int partIndex = 0; partIndex < partCount; ++partIndex) {
                    auto name = LoadSuspended<TString>(context);
                    int version = LoadSuspended<i32>(context);
                    SERIALIZATION_DUMP_WRITE(context, "%v@%v =>", name, version);

                    SERIALIZATION_DUMP_INDENT(context) {
                        auto* checkpointableInput = context.GetCheckpointableInput();
                        auto readPart = [&] (auto func) {
                            auto offsetBefore = checkpointableInput->GetOffset();
                            func();
                            checkpointableInput->SkipToCheckpoint();
                            auto offsetAfter = checkpointableInput->GetOffset();
                            return offsetAfter - offsetBefore;
                        };

                        auto it = PartNameToLoaderDescriptor_.find(name);
                        if (it == PartNameToLoaderDescriptor_.end()) {
                            SERIALIZATION_DUMP_WRITE(context, "<skipped>");
                            YT_LOG_INFO("Started skipping unknown automaton part (Name: %v, Version: %v)",
                                name,
                                version);
                            auto size = readPart([] { });
                            YT_LOG_INFO("Finished skipping unknown automaton part (Name: %v, Size: %v)",
                                name,
                                size);
                        } else {
                            YT_LOG_INFO("Started loading automaton part (Name: %v, Version: %v)",
                                name,
                                version);
                            context.SetVersion(version);
                            const auto& descriptor = it->second;
                            auto size = readPart([&] { descriptor.Callback.Run(context); });
                            YT_LOG_INFO("Finished loading automaton part (Name: %v, Size: %v)",
                                name,
                                size);
                        }
                    }
                }
            }

            if (context.GetEnableTotalWriteCountReport()) {
                context.Dumper().ReportWriteCount();
            }

            for (const auto& part : parts) {
                part->OnAfterSnapshotLoaded();
            }
        });
}

void TCompositeAutomaton::RememberReign(TReign reign)
{
    auto recoveryAction = GetActionToRecoverFromReign(reign);

    YT_VERIFY(IsRecovery() || recoveryAction == EFinalRecoveryAction::None);

    if (recoveryAction != FinalRecoveryAction_) {
        YT_LOG_DEBUG("Updating final recovery action (MutationReign: %v, CurrentFinalRecoveryAction: %v, MutationFinalRecoveryAction: %v)",
            reign,
            FinalRecoveryAction_,
            recoveryAction);
        FinalRecoveryAction_ = std::max(FinalRecoveryAction_, recoveryAction);
    }
}

void TCompositeAutomaton::ApplyMutation(TMutationContext* context)
{
    const auto& request = context->Request();
    const auto& mutationType = request.Type;
    auto mutationId = request.MutationId;
    auto version = context->GetVersion();
    auto isRecovery = IsRecovery();
    auto waitTime = GetInstant() - context->GetTimestamp();

    // COMPAT(savrus) Skip unreigned heartbeat mutations which are already in changelog.
    if (mutationType != HeartbeatMutationType) {
        RememberReign(request.Reign);
    }

    if (!isRecovery) {
        Profiler.Update(MutationWaitTimeCounter_, DurationToValue(waitTime));
    }

    if (mutationType.empty()) {
        YT_LOG_DEBUG_UNLESS(isRecovery, "Skipping heartbeat mutation (Version: %v)",
            version);
    } else {
        NProfiling::TWallTimer timer;

        YT_LOG_DEBUG_UNLESS(isRecovery, "Applying mutation (Version: %v, SequenceNumber: %v, RandomSeed: %llx, PrevRandomSeed: %llx, MutationType: %v, MutationId: %v, WaitTime: %v)",
            version,
            context->GetSequenceNumber(),
            context->GetRandomSeed(),
            context->GetPrevRandomSeed(),
            mutationType,
            mutationId,
            waitTime);

        auto* descriptor = GetMethodDescriptor(mutationType);
        const auto& handler = request.Handler;
        if (handler) {
            handler.Run(context);
        } else {
            descriptor->Callback.Run(context);
        }

        if (!isRecovery) {
            Profiler.Increment(descriptor->CumulativeTimeCounter, DurationToValue(timer.GetElapsedTime()));
        }
    }

    if (!isRecovery) {
        Profiler.Increment(MutationCounter_);
    }
}

void TCompositeAutomaton::Clear()
{
    for (const auto& part : GetParts()) {
        part->Clear();
    }
}

void TCompositeAutomaton::SetZeroState()
{
    for (const auto& part : GetParts()) {
        part->SetZeroState();
    }
}

void TCompositeAutomaton::DoSaveSnapshot(
    NConcurrency::IAsyncOutputStreamPtr writer,
    ESyncStreamAdapterStrategy strategy,
    const std::function<void(TSaveContext&)>& callback)
{
    auto syncWriter = CreateBufferedCheckpointableSyncAdapter(writer, strategy, SnapshotSaveBufferSize);
    auto context = CreateSaveContext(syncWriter.get());
    callback(*context);
}

void TCompositeAutomaton::DoLoadSnapshot(
    IAsyncZeroCopyInputStreamPtr reader,
    const std::function<void(TLoadContext&)>& callback)
{
    auto prefetchingReader = CreatePrefetchingAdapter(reader, SnapshotPrefetchWindowSize);
    auto copyingReader = CreateCopyingAdapter(prefetchingReader);
    auto syncReader = CreateSyncAdapter(copyingReader, ESyncStreamAdapterStrategy::Get);
    TBufferedInput bufferedInput(syncReader.get(), SnapshotLoadBufferSize);
    auto checkpointableInput = CreateCheckpointableInputStream(&bufferedInput);
    auto context = CreateLoadContext(checkpointableInput.get());
    callback(*context);
}

void TCompositeAutomaton::WritePartHeader(TSaveContext& context, const TSaverDescriptorBase& descriptor)
{
    auto version = descriptor.SnapshotVersion;
    YT_LOG_INFO("Saving automaton part (Name: %v, Version: %v)",
        descriptor.Name,
        version);

    context.GetCheckpointableOutput()->MakeCheckpoint();

    Save(context, descriptor.Name);
    Save<i32>(context, version);
}

void TCompositeAutomaton::OnRecoveryStarted()
{
    Profiler.SetEnabled(false);
}

void TCompositeAutomaton::OnRecoveryComplete()
{
    Profiler.SetEnabled(true);
}

TCompositeAutomaton::TMethodDescriptor* TCompositeAutomaton::GetMethodDescriptor(const TString& mutationType)
{
    return &GetOrCrash(MethodNameToDescriptor_, mutationType);
}

std::vector<TCompositeAutomatonPartPtr> TCompositeAutomaton::GetParts()
{
    std::vector<TCompositeAutomatonPartPtr> parts;
    for (const auto& weakPart : Parts_) {
        auto strongPart = weakPart.Lock();
        if (strongPart) {
            parts.push_back(strongPart);
        }
    }
    return parts;
}

void TCompositeAutomaton::LogHandlerError(const TError& error)
{
    YT_LOG_DEBUG(error, "Error executing mutation handler");
}

bool TCompositeAutomaton::IsRecovery() const
{
    return HydraManager_->IsRecovery();
}

EFinalRecoveryAction TCompositeAutomaton::GetFinalRecoveryAction()
{
    return FinalRecoveryAction_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
