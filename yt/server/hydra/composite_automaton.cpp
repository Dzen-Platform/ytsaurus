#include "composite_automaton.h"
#include "private.h"
#include "hydra_manager.h"
#include "mutation_context.h"
#include "snapshot.h"

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/serialize.h>

#include <util/stream/buffered.h>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

static const size_t SnapshotLoadBufferSize = 64 * 1024;
static const size_t SnapshotSaveBufferSize = 64 * 1024;
static const size_t SnapshotPrefetchWindowSize = 64 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////

TCompositeAutomatonPart::TCompositeAutomatonPart(
    IHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton,
    IInvokerPtr automatonInvoker)
    : HydraManager_(std::move(hydraManager))
    , Automaton_(automaton.Get())
    , AutomatonInvoker_(std::move(automatonInvoker))
{
    YCHECK(HydraManager_);
    YCHECK(Automaton_);

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
    const Stroka& name,
    TCallback<void(TSaveContext&)> callback)
{
    // Check for duplicate part names.
    YCHECK(Automaton_->SaverPartNames_.insert(name).second);

    TCompositeAutomaton::TSyncSaverDescriptor descriptor;
    descriptor.Priority = priority;
    descriptor.Name = name;
    descriptor.Callback = callback;
    descriptor.SnapshotVersion = GetCurrentSnapshotVersion();
    Automaton_->SyncSavers_.push_back(descriptor);
}

void TCompositeAutomatonPart::RegisterSaver(
    EAsyncSerializationPriority priority,
    const Stroka& name,
    TCallback<TCallback<void(TSaveContext&)>()> callback)
{
    // Check for duplicate part names.
    YCHECK(Automaton_->SaverPartNames_.insert(name).second);

    TCompositeAutomaton::TAsyncSaverDescriptor descriptor;
    descriptor.Priority = priority;
    descriptor.Name = name;
    descriptor.Callback = callback;
    descriptor.SnapshotVersion = GetCurrentSnapshotVersion();
    Automaton_->AsyncSavers_.push_back(descriptor);
}

void TCompositeAutomatonPart::RegisterLoader(
    const Stroka& name,
    TCallback<void(TLoadContext&)> callback)
{
    TCompositeAutomaton::TLoaderDescriptor descriptor;
    descriptor.Name = name;
    descriptor.Callback = BIND([=] (TLoadContext& context) {
        if (!ValidateSnapshotVersion(context.GetVersion())) {
            THROW_ERROR_EXCEPTION("Unsupported snapshot version %v in part %v",
                context.GetVersion(),
                name);
        }
        callback.Run(context);
    });
    YCHECK(Automaton_->PartNameToLoaderDescriptor_.insert(std::make_pair(name, descriptor)).second);
}

void TCompositeAutomatonPart::RegisterMethod(
    const Stroka& type,
    TCallback<void(TMutationContext*)> callback)
{
    TCompositeAutomaton::TMethodDescriptor descriptor{
        callback
    };
    YCHECK(Automaton_->MethodNameToDescriptor_.insert(std::make_pair(type, descriptor)).second);
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
}

void TCompositeAutomatonPart::StopEpoch()
{
    EpochAutomatonInvoker_.Reset();
}

////////////////////////////////////////////////////////////////////////////////

TCompositeAutomaton::TCompositeAutomaton(IInvokerPtr asyncSnapshotInvoker)
    : Logger(HydraLogger)
    , Profiler(HydraProfiler)
    , AsyncSnapshotInvoker_(asyncSnapshotInvoker)
{ }

void TCompositeAutomaton::SetSerializationDumpEnabled(bool value)
{
    SerializationDumpEnabled_ = value;
}

void TCompositeAutomaton::RegisterPart(TCompositeAutomatonPartPtr part)
{
    YCHECK(part);

    Parts_.push_back(part);

    if (Parts_.size() == 1) {
        auto hydraManager = part->HydraManager_;

        hydraManager->SubscribeStartLeading(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
        hydraManager->SubscribeLeaderRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));

        hydraManager->SubscribeStartFollowing(BIND(&TThis::OnRecoveryStarted, MakeWeak(this)));
        hydraManager->SubscribeFollowerRecoveryComplete(BIND(&TThis::OnRecoveryComplete, MakeWeak(this)));
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

    YCHECK(AsyncSnapshotInvoker_);

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
    auto paxrts = GetParts();
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
                    auto name = LoadSuspended<Stroka>(context);
                    int version = LoadSuspended<i32>(context);

                    SERIALIZATION_DUMP_WRITE(context, "%v@%v =>", name, version);
                    SERIALIZATION_DUMP_INDENT(context) {
                        auto it = PartNameToLoaderDescriptor_.find(name);
                        if (it == PartNameToLoaderDescriptor_.end()) {
                            SERIALIZATION_DUMP_WRITE(context, "<skipped>");
                            LOG_INFO("Skipping unknown automaton part (Name: %v, Version: %v)",
                                name,
                                version);
                        } else {
                            LOG_INFO("Loading automaton part (Name: %v, Version: %v)",
                                name,
                                version);
                            context.SetVersion(version);
                            const auto& descriptor = it->second;
                            descriptor.Callback.Run(context);
                        }
                    }

                    context.GetCheckpointableInput()->SkipToCheckpoint();
                }
            }

            for (const auto& part : parts) {
                part->OnAfterSnapshotLoaded();
            }
        });
}

void TCompositeAutomaton::ApplyMutation(TMutationContext* context)
{
    const auto& type = context->Request().Type;
    if (type.empty()) {
        // Empty mutation. Typically appears as a tombstone after editing changelogs.
        return;
    }

    auto it = MethodNameToDescriptor_.find(type);
    YCHECK(it != MethodNameToDescriptor_.end());
    const auto& descriptor = it->second;
    descriptor.Callback.Run(context);
}

void TCompositeAutomaton::Clear()
{
    for (const auto& part : GetParts()) {
        part->Clear();
    }
}

void TCompositeAutomaton::DoSaveSnapshot(
    NConcurrency::IAsyncOutputStreamPtr writer,
    ESyncStreamAdapterStrategy strategy,
    const std::function<void(TSaveContext&)>& callback)
{
    auto syncWriter = CreateSyncAdapter(writer, strategy);
    auto checkpointableOutput = CreateCheckpointableOutputStream(syncWriter.get());
    auto bufferedCheckpointableOutput = CreateBufferedCheckpointableOutputStream(
        checkpointableOutput.get(),
        SnapshotSaveBufferSize);
    auto context = CreateSaveContext(bufferedCheckpointableOutput.get());
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
    context.GetCheckpointableOutput()->MakeCheckpoint();

    auto version = descriptor.SnapshotVersion;
    LOG_INFO("Saving automaton part (Name: %v, Version: %v)",
        descriptor.Name,
        version);

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

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
