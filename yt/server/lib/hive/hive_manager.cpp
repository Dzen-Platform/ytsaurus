#include "hive_manager.h"
#include "config.h"
#include "mailbox.h"
#include "helpers.h"
#include "private.h"

#include <yt/server/lib/election/election_manager.h>

#include <yt/server/lib/hydra/composite_automaton.h>
#include <yt/server/lib/hydra/hydra_manager.h>
#include <yt/server/lib/hydra/hydra_service.h>
#include <yt/server/lib/hydra/mutation.h>
#include <yt/server/lib/hydra/mutation_context.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/hive_service_proxy.h>

#include <yt/ytlib/hydra/config.h>
#include <yt/ytlib/hydra/peer_channel.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/fls.h>
#include <yt/core/concurrency/async_batcher.h>

#include <yt/core/net/local_address.h>

#include <yt/core/rpc/proto/rpc.pb.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/service_detail.h>

#include <yt/core/tracing/trace_context.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NHiveServer {

using namespace NNet;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NHydra;
using namespace NHydra::NProto;
using namespace NHiveClient;
using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NTracing;
using namespace NProfiling;

using NYT::ToProto;
using NYT::FromProto;

using NHiveClient::NProto::TEncapsulatedMessage;

////////////////////////////////////////////////////////////////////////////////

static const auto& Profiler = HiveServerProfiler;

////////////////////////////////////////////////////////////////////////////////

static NConcurrency::TFls<bool> HiveMutation;

bool IsHiveMutation()
{
    return *HiveMutation;
}

class THiveMutationGuard
    : private TNonCopyable
{
public:
    THiveMutationGuard()
    {
        YT_ASSERT(!*HiveMutation);
        *HiveMutation = true;
    }

    ~THiveMutationGuard()
    {
        *HiveMutation = false;
    }
};

////////////////////////////////////////////////////////////////////////////////

class THiveManager::TImpl
    : public THydraServiceBase
    , public TCompositeAutomatonPart
{
public:
    TImpl(
        THiveManagerConfigPtr config,
        TCellDirectoryPtr cellDirectory,
        TCellId selfCellId,
        IInvokerPtr automatonInvoker,
        IHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton)
        : THydraServiceBase(
            hydraManager->CreateGuardedAutomatonInvoker(automatonInvoker),
            THiveServiceProxy::GetDescriptor(),
            HiveServerLogger,
            selfCellId)
        , TCompositeAutomatonPart(
            hydraManager,
            automaton,
            automatonInvoker)
        , SelfCellId_(selfCellId)
        , Config_(std::move(config))
        , CellDirectory_(std::move(cellDirectory))
        , AutomatonInvoker_(std::move(automatonInvoker))
        , GuardedAutomatonInvoker_(hydraManager->CreateGuardedAutomatonInvoker(AutomatonInvoker_))
        , HydraManager_(std::move(hydraManager))
    {
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(Ping));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(SyncCells));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(PostMessages));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(SendMessages));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(SyncWithOthers));

        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraAcknowledgeMessages, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraPostMessages, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraSendMessages, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraUnregisterMailbox, Unretained(this)));

        RegisterLoader(
            "HiveManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "HiveManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "HiveManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "HiveManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        OrchidService_ = CreateOrchidService();
    }

    IServicePtr GetRpcService()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return this;
    }

    IYPathServicePtr GetOrchidService()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return OrchidService_;
    }

    TCellId GetSelfCellId() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return SelfCellId_;
    }

    TMailbox* CreateMailbox(TCellId cellId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto mailboxHolder = std::make_unique<TMailbox>(cellId);
        auto* mailbox = MailboxMap_.Insert(cellId, std::move(mailboxHolder));

        if (!IsRecovery()) {
            SendPeriodicPing(mailbox);
        }

        YT_LOG_INFO_UNLESS(IsRecovery(), "Mailbox created (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            mailbox->GetCellId());
        return mailbox;
    }

    TMailbox* GetOrCreateMailbox(TCellId cellId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* mailbox = MailboxMap_.Find(cellId);
        if (!mailbox) {
            mailbox = CreateMailbox(cellId);
        }
        return mailbox;
    }

    TMailbox* GetMailboxOrThrow(TCellId cellId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            THROW_ERROR_EXCEPTION("No such mailbox %v",
                cellId);
        }
        return mailbox;
    }

    void RemoveMailbox(TMailbox* mailbox)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = mailbox->GetCellId();
        MailboxMap_.Remove(cellId);
        YT_LOG_INFO_UNLESS(IsRecovery(), "Mailbox removed (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            cellId);
    }

    void PostMessage(TMailbox* mailbox, TRefCountedEncapsulatedMessagePtr message, bool reliable)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        PostMessage(TMailboxList{mailbox}, std::move(message), reliable);
    }

    void PostMessage(const TMailboxList& mailboxes, TRefCountedEncapsulatedMessagePtr message, bool reliable)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (reliable) {
            ReliablePostMessage(mailboxes, std::move(message));
        } else {
            UnreliablePostMessage(mailboxes, std::move(message));
        }
    }

    void PostMessage(TMailbox* mailbox, const ::google::protobuf::MessageLite& message, bool reliable)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto encapsulatedMessage = SerializeMessage(message);
        PostMessage(mailbox, std::move(encapsulatedMessage), reliable);
    }

    void PostMessage(const TMailboxList& mailboxes, const ::google::protobuf::MessageLite& message, bool reliable)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto encapsulatedMessage = SerializeMessage(message);
        PostMessage(mailboxes, std::move(encapsulatedMessage), reliable);
    }

    TFuture<void> SyncWith(TCellId cellId, bool enableBatching)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (enableBatching) {
            return GetOrCreateSyncBatcher(cellId)->Run();
        } else {
            return DoSyncWithCore(cellId);
        }
    }

    DEFINE_SIGNAL(TFuture<void>(NHiveClient::TCellId srcCellId), IncomingMessageUpstreamSync);

    DECLARE_ENTITY_MAP_ACCESSORS(Mailbox, TMailbox)

private:
    const TCellId SelfCellId_;
    const THiveManagerConfigPtr Config_;
    const TCellDirectoryPtr CellDirectory_;
    const IInvokerPtr AutomatonInvoker_;
    const IInvokerPtr GuardedAutomatonInvoker_;
    const IHydraManagerPtr HydraManager_;

    IYPathServicePtr OrchidService_;

    TEntityMap<TMailbox> MailboxMap_;
    THashMap<TCellId, TMessageId> CellIdToNextTransientIncomingMessageId_;

    TReaderWriterSpinLock CellToIdToBatcherLock_;
    THashMap<TCellId, TIntrusivePtr<TAsyncBatcher<void>>> CellToIdToBatcher_;

    TMonotonicCounter PostingTimeCounter_{"/posting_time"};

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    // RPC handlers.

    DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto, Ping)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellId = FromProto<TCellId>(request->src_cell_id());

        context->SetRequestInfo("SrcCellId: %v, DstCellId: %v",
            srcCellId,
            SelfCellId_);

        ValidatePeer(EPeerKind::Leader);

        auto* mailbox = FindMailbox(srcCellId);
        auto lastOutcomingMessageId = mailbox
            ? std::make_optional(mailbox->GetFirstOutcomingMessageId() + static_cast<int>(mailbox->OutcomingMessages().size()) - 1)
            : std::nullopt;

        if (lastOutcomingMessageId) {
            response->set_last_outcoming_message_id(*lastOutcomingMessageId);
        }

        context->SetResponseInfo("NextTransientIncomingMessageId: %v",
            lastOutcomingMessageId);

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto, SyncCells)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        context->SetRequestInfo();

        ValidatePeer(EPeerKind::LeaderOrFollower);
        SyncWithUpstream();

        auto registeredCellList = CellDirectory_->GetRegisteredCells();
        THashMap<TCellId, TCellInfo> registeredCellMap;
        for (const auto& cellInfo : registeredCellList) {
            YT_VERIFY(registeredCellMap.insert(std::make_pair(cellInfo.CellId, cellInfo)).second);
        }

        THashSet<TCellId> missingCellIds;
        for (const auto& cellInfo : registeredCellList) {
            YT_VERIFY(missingCellIds.insert(cellInfo.CellId).second);
        }

        auto requestReconfigure = [&] (const TCellDescriptor& cellDescriptor, int oldVersion) {
            YT_LOG_DEBUG("Requesting cell reconfiguration (CellId: %v, ConfigVersion: %v -> %v)",
                cellDescriptor.CellId,
                oldVersion,
                cellDescriptor.ConfigVersion);
            auto* protoInfo = response->add_cells_to_reconfigure();
            ToProto(protoInfo->mutable_cell_descriptor(), cellDescriptor);
        };

        auto requestUnregister = [&] (TCellId cellId) {
            YT_LOG_DEBUG("Requesting cell unregistration (CellId: %v)",
                cellId);
            auto* unregisterInfo = response->add_cells_to_unregister();
            ToProto(unregisterInfo->mutable_cell_id(), cellId);
        };

        for (const auto& protoCellInfo : request->known_cells()) {
            auto cellId = FromProto<TCellId>(protoCellInfo.cell_id());
            auto it = registeredCellMap.find(cellId);
            if (it == registeredCellMap.end()) {
                requestUnregister(cellId);
            } else {
                YT_VERIFY(missingCellIds.erase(cellId) == 1);
                const auto& cellInfo = it->second;
                if (protoCellInfo.config_version() < cellInfo.ConfigVersion) {
                    auto cellDescriptor = CellDirectory_->FindDescriptor(cellId);
                    // If cell descriptor is already missing then just skip this cell and
                    // postpone it for another heartbeat.
                    if (cellDescriptor) {
                        requestReconfigure(*cellDescriptor, protoCellInfo.config_version());
                    }
                }
            }
        }

        for (auto cellId : missingCellIds) {
            auto cellDescriptor = CellDirectory_->FindDescriptor(cellId);
            // See above.
            if (cellDescriptor) {
                requestReconfigure(*cellDescriptor, -1);
            }
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto, PostMessages)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellId = FromProto<TCellId>(request->src_cell_id());
        auto firstMessageId = request->first_message_id();
        int messageCount = request->messages_size();

        context->SetRequestInfo("SrcCellId: %v, DstCellId: %v, MessageIds: %v-%v",
            srcCellId,
            SelfCellId_,
            firstMessageId,
            firstMessageId + messageCount - 1);

        ValidatePeer(EPeerKind::Leader);
        SyncWithUpstreamOnIncomingMessage(srcCellId);

        auto* nextTransientIncomingMessageId = GetNextTransientIncomingMessageIdPtr(srcCellId);
        if (*nextTransientIncomingMessageId == firstMessageId && messageCount > 0) {
            YT_LOG_DEBUG_UNLESS(IsRecovery(), "Committing reliable incoming messages (SrcCellId: %v, DstCellId: %v, "
                "MessageIds: %v-%v)",
                srcCellId,
                SelfCellId_,
                firstMessageId,
                firstMessageId + messageCount - 1);

            *nextTransientIncomingMessageId += messageCount;
            CreatePostMessagesMutation(*request)
                ->CommitAndLog(Logger);
        }
        response->set_next_transient_incoming_message_id(*nextTransientIncomingMessageId);

        auto nextPersistentIncomingMessageId = GetNextPersistentIncomingMessageId(srcCellId);
        if (nextPersistentIncomingMessageId) {
            response->set_next_persistent_incoming_message_id(*nextPersistentIncomingMessageId);
        }

        context->SetResponseInfo("NextPersistentIncomingMessageId: %v, NextTransientIncomingMessageId: %v",
            nextPersistentIncomingMessageId,
            *nextTransientIncomingMessageId);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto, SendMessages)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellId = FromProto<TCellId>(request->src_cell_id());
        int messageCount = request->messages_size();

        context->SetRequestInfo("SrcCellId: %v, DstCellId: %v, MessageCount: %v",
            srcCellId,
            SelfCellId_,
            messageCount);

        ValidatePeer(EPeerKind::Leader);
        SyncWithUpstreamOnIncomingMessage(srcCellId);

        YT_LOG_DEBUG_UNLESS(IsRecovery(), "Committing unreliable incoming messages (SrcCellId: %v, DstCellId: %v, "
            "MessageCount: %v)",
            srcCellId,
            SelfCellId_,
            messageCount);

        CreateSendMessagesMutation(context)
            ->CommitAndReply(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto, SyncWithOthers)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellIds = FromProto<std::vector<TCellId>>(request->src_cell_ids());

        context->SetRequestInfo("SrcCellIds: %v",
            srcCellIds);

        ValidatePeer(EPeerKind::Leader);

        std::vector<TFuture<void>> asyncResults;
        for (auto cellId : srcCellIds) {
            asyncResults.push_back(SyncWith(cellId, true));
        }

        context->ReplyFrom(Combine(asyncResults));
    }


    // Hydra handlers.

    void HydraAcknowledgeMessages(NHiveServer::NProto::TReqAcknowledgeMessages* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TCellId>(request->cell_id());
        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        mailbox->SetAcknowledgeInProgress(false);

        auto nextPersistentIncomingMessageId = request->next_persistent_incoming_message_id();
        auto acknowledgeCount = nextPersistentIncomingMessageId - mailbox->GetFirstOutcomingMessageId();
        if (acknowledgeCount <= 0) {
            YT_LOG_DEBUG_UNLESS(IsRecovery(), "No messages acknowledged (SrcCellId: %v, DstCellId: %v, "
                "NextPersistentIncomingMessageId: %v, FirstOutcomingMessageId: %v)",
                SelfCellId_,
                mailbox->GetCellId(),
                nextPersistentIncomingMessageId,
                mailbox->GetFirstOutcomingMessageId());
            return;
        }

        auto& outcomingMessages = mailbox->OutcomingMessages();
        if (acknowledgeCount > outcomingMessages.size()) {
            YT_LOG_ERROR_UNLESS(IsRecovery(), "Requested to acknowledge too many messages (SrcCellId: %v, DstCellId: %v, "
                "NextPersistentIncomingMessageId: %v, FirstOutcomingMessageId: %v, OutcomingMessageCount: %v)",
                SelfCellId_,
                mailbox->GetCellId(),
                nextPersistentIncomingMessageId,
                mailbox->GetFirstOutcomingMessageId(),
                outcomingMessages.size());
            return;
        }

        outcomingMessages.erase(outcomingMessages.begin(), outcomingMessages.begin() + acknowledgeCount);
        mailbox->SetFirstOutcomingMessageId(mailbox->GetFirstOutcomingMessageId() + acknowledgeCount);

        YT_LOG_DEBUG_UNLESS(IsRecovery(), "Messages acknowledged (SrcCellId: %v, DstCellId: %v, "
            "FirstOutcomingMessageId: %v)",
            SelfCellId_,
            mailbox->GetCellId(),
            mailbox->GetFirstOutcomingMessageId());
    }

    void HydraPostMessages(NHiveClient::NProto::TReqPostMessages* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellId = FromProto<TCellId>(request->src_cell_id());
        auto firstMessageId = request->first_message_id();
        auto* mailbox = FindMailbox(srcCellId);
        if (!mailbox) {
            if (firstMessageId != 0) {
                YT_LOG_ERROR_UNLESS(IsRecovery(), "Mailbox %v does not exist; expecting message 0 but got %v",
                    srcCellId,
                    firstMessageId);
                return;
            }
            mailbox = CreateMailbox(srcCellId);
        }

        ApplyReliableIncomingMessages(mailbox, request);
    }

    void HydraSendMessages(
        const TCtxSendMessagesPtr& /*context*/,
        NHiveClient::NProto::TReqSendMessages* request,
        NHiveClient::NProto::TRspSendMessages* /*response*/)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto srcCellId = FromProto<TCellId>(request->src_cell_id());
        auto* mailbox = GetMailboxOrThrow(srcCellId);
        ApplyUnreliableIncomingMessages(mailbox, request);
    }

    void HydraUnregisterMailbox(NHiveServer::NProto::TReqUnregisterMailbox* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TCellId>(request->cell_id());
        auto* mailbox = FindMailbox(cellId);
        if (mailbox) {
            RemoveMailbox(mailbox);
        }
    }


    NRpc::IChannelPtr FindMailboxChannel(TMailbox* mailbox)
    {
        auto now = GetCpuInstant();
        auto cachedChannel = mailbox->GetCachedChannel();
        if (cachedChannel && now < mailbox->GetCachedChannelDeadline()) {
            return cachedChannel;
        }

        auto channel = CellDirectory_->FindChannel(mailbox->GetCellId());
        if (!channel) {
            return nullptr;
        }

        mailbox->SetCachedChannel(channel);
        mailbox->SetCachedChannelDeadline(now + DurationToCpuDuration(Config_->CachedChannelTimeout));

        return channel;
    }

    void ReliablePostMessage(const TMailboxList& mailboxes, const TRefCountedEncapsulatedMessagePtr& message)
    {
        // A typical mistake is posting a reliable Hive message outside of a mutation.
        YT_VERIFY(HasMutationContext());

        AnnotateWithTraceContext(message.Get());

        TStringBuilder logMessageBuilder;
        logMessageBuilder.AppendFormat("Reliable outcoming message added (MutationType: %v, SrcCellId: %v, DstCellIds: {",
            message->type(),
            SelfCellId_);

        for (auto* mailbox : mailboxes) {
            auto messageId =
                mailbox->GetFirstOutcomingMessageId() +
                mailbox->OutcomingMessages().size();

            mailbox->OutcomingMessages().push_back(message);

            if (mailbox != mailboxes.front()) {
                logMessageBuilder.AppendString(AsStringBuf(", "));
            }
            logMessageBuilder.AppendFormat("%v=>%v",
                mailbox->GetCellId(),
                messageId);

            SchedulePostOutcomingMessages(mailbox);
        }

        logMessageBuilder.AppendString(AsStringBuf("})"));
        YT_LOG_DEBUG_UNLESS(IsRecovery(), logMessageBuilder.Flush());
    }

    void UnreliablePostMessage(const TMailboxList& mailboxes, const TRefCountedEncapsulatedMessagePtr& message)
    {
        TCounterIncrementingTimingGuard<TWallTimer> timingGuard(Profiler, &PostingTimeCounter_);

        TStringBuilder logMessageBuilder;
        logMessageBuilder.AppendFormat("Sending unreliable outcoming message (MutationType: %v, SrcCellId: %v, DstCellIds: [",
            message->type(),
            SelfCellId_);

        for (auto* mailbox : mailboxes) {
            if (!mailbox->GetConnected()) {
                continue;
            }

            auto channel = FindMailboxChannel(mailbox);
            if (!channel) {
                continue;
            }

            if (mailbox != mailboxes.front()) {
                logMessageBuilder.AppendString(AsStringBuf(", "));
            }
            logMessageBuilder.AppendFormat("%v", mailbox->GetCellId());

            THiveServiceProxy proxy(std::move(channel));
            auto req = proxy.SendMessages();
            req->SetTimeout(Config_->SendRpcTimeout);
            ToProto(req->mutable_src_cell_id(), SelfCellId_);
            req->add_messages()->CopyFrom(*message);
            AnnotateWithTraceContext(req->mutable_messages(0));

            req->Invoke().Subscribe(
                BIND(&TImpl::OnSendMessagesResponse, MakeStrong(this), mailbox->GetCellId())
                    .Via(EpochAutomatonInvoker_));
        }

        logMessageBuilder.AppendString(AsStringBuf("])"));
        YT_LOG_DEBUG_UNLESS(IsRecovery(), logMessageBuilder.Flush());
    }


    void SetMailboxConnected(TMailbox* mailbox)
    {
        if (mailbox->GetConnected()) {
            return;
        }

        mailbox->SetConnected(true);
        YT_VERIFY(mailbox->SyncRequests().empty());
        mailbox->SetFirstInFlightOutcomingMessageId(mailbox->GetFirstOutcomingMessageId());
        YT_VERIFY(mailbox->GetInFlightOutcomingMessageCount() == 0);

        YT_LOG_INFO("Mailbox connected (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            mailbox->GetCellId());

        PostOutcomingMessages(mailbox, true);
    }

    void SetMailboxDisconnected(TMailbox* mailbox)
    {
        if (!mailbox->GetConnected()) {
            return;
        }

        mailbox->SetConnected(false);
        mailbox->SetPostInProgress(false);
        mailbox->SyncRequests().clear();
        mailbox->SetFirstInFlightOutcomingMessageId(mailbox->GetFirstOutcomingMessageId());
        mailbox->SetInFlightOutcomingMessageCount(0);
        TDelayedExecutor::CancelAndClear(mailbox->IdlePostCookie());

        YT_LOG_INFO("Mailbox disconnected (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            mailbox->GetCellId());
    }

    void ResetMailboxes()
    {
        decltype(CellToIdToBatcher_) cellToIdToBatcher;
        {
            TWriterGuard guard(CellToIdToBatcherLock_);
            std::swap(cellToIdToBatcher, CellToIdToBatcher_);
        }

        auto error = TError(NRpc::EErrorCode::Unavailable, "Hydra peer has stopped");
        for (const auto& [cellId, batcher] : cellToIdToBatcher) {
            batcher->Cancel(error);
        }

        for (auto [id, mailbox] : MailboxMap_) {
            SetMailboxDisconnected(mailbox);
            mailbox->SetAcknowledgeInProgress(false);
            mailbox->SetCachedChannel(nullptr);
            mailbox->SetPostBatchingCookie(nullptr);
        }
        CellIdToNextTransientIncomingMessageId_.clear();
    }


    TMessageId* GetNextTransientIncomingMessageIdPtr(TCellId cellId)
    {
        auto it = CellIdToNextTransientIncomingMessageId_.find(cellId);
        if (it != CellIdToNextTransientIncomingMessageId_.end()) {
            return &it->second;
        }

        return &CellIdToNextTransientIncomingMessageId_.emplace(
            cellId,
            GetNextPersistentIncomingMessageId(cellId).value_or(0)).first->second;
    }

    TMessageId GetNextTransientIncomingMessageId(TMailbox* mailbox)
    {
        auto it = CellIdToNextTransientIncomingMessageId_.find(mailbox->GetCellId());
        return it == CellIdToNextTransientIncomingMessageId_.end()
            ? mailbox->GetNextIncomingMessageId()
            : it->second;
    }

    std::optional<TMessageId> GetNextPersistentIncomingMessageId(TCellId cellId)
    {
        auto* mailbox = FindMailbox(cellId);
        return mailbox ? std::make_optional(mailbox->GetNextIncomingMessageId()) : std::nullopt;
    }


    void SchedulePeriodicPing(TMailbox* mailbox)
    {
        TDelayedExecutor::Submit(
            BIND(&TImpl::OnPeriodicPingTick, MakeWeak(this), mailbox->GetCellId())
                .Via(EpochAutomatonInvoker_),
            Config_->PingPeriod);
    }

    void ReconnectMailboxes()
    {
        for (const auto& pair : MailboxMap_) {
            auto* mailbox = pair.second;
            YT_VERIFY(!mailbox->GetConnected());
            SendPeriodicPing(mailbox);
        }
    }

    void OnPeriodicPingTick(TCellId cellId)
    {
        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        SendPeriodicPing(mailbox);
    }

    void SendPeriodicPing(TMailbox* mailbox)
    {
        auto cellId = mailbox->GetCellId();

        if (IsLeader() && CellDirectory_->IsCellUnregistered(cellId)) {
            NHiveServer::NProto::TReqUnregisterMailbox req;
            ToProto(req.mutable_cell_id(), cellId);
            CreateUnregisterMailboxMutation(req)
                ->CommitAndLog(Logger);
            return;
        }

        if (mailbox->GetConnected()) {
            SchedulePeriodicPing(mailbox);
            return;
        }

        auto channel = FindMailboxChannel(mailbox);
        if (!channel) {
            // Let's register a dummy descriptor so as to ask about it during the next sync.
            CellDirectory_->RegisterCell(cellId);
            SchedulePeriodicPing(mailbox);
            return;
        }

        YT_LOG_DEBUG("Sending periodic ping (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            mailbox->GetCellId());

        THiveServiceProxy proxy(std::move(channel));
        auto req = proxy.Ping();
        req->SetTimeout(Config_->PingRpcTimeout);
        ToProto(req->mutable_src_cell_id(), SelfCellId_);

        req->Invoke().Subscribe(
            BIND(&TImpl::OnPeriodicPingResponse, MakeStrong(this), mailbox->GetCellId())
                .Via(EpochAutomatonInvoker_));
    }

    void OnPeriodicPingResponse(TCellId cellId, const THiveServiceProxy::TErrorOrRspPingPtr& rspOrError)
    {
        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        SchedulePeriodicPing(mailbox);

        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Periodic ping failed (SrcCellId: %v, DstCellId: %v)",
                SelfCellId_,
                mailbox->GetCellId());
            return;
        }

        const auto& rsp = rspOrError.Value();
        auto lastOutcomingMessageId = rsp->has_last_outcoming_message_id()
            ? std::make_optional(rsp->last_outcoming_message_id())
            : std::nullopt;

        YT_LOG_DEBUG("Periodic ping succeeded (SrcCellId: %v, DstCellId: %v, LastOutcomingMessageId: %v)",
            SelfCellId_,
            mailbox->GetCellId(),
            lastOutcomingMessageId);

        SetMailboxConnected(mailbox);
    }


    TIntrusivePtr<TAsyncBatcher<void>> GetOrCreateSyncBatcher(TCellId cellId)
    {
        {
            TReaderGuard readerGuard(CellToIdToBatcherLock_);
            auto it = CellToIdToBatcher_.find(cellId);
            if (it != CellToIdToBatcher_.end()) {
                return it->second;
            }
        }

        auto batcher = New<TAsyncBatcher<void>>(
            BIND(&TImpl::DoSyncWith, MakeWeak(this), cellId),
            Config_->SyncDelay);

        {
            TWriterGuard writerGuard(CellToIdToBatcherLock_);
            auto it = CellToIdToBatcher_.emplace(cellId, std::move(batcher)).first;
            return it->second;
        }
    }

    static TFuture<void> DoSyncWith(const TWeakPtr<TImpl>& weakThis, TCellId cellId)
    {
        auto this_ = weakThis.Lock();
        if (!this_) {
            return MakeFuture(TError(NRpc::EErrorCode::Unavailable, "Hydra peer has stopped"));
        }

        return this_->DoSyncWithCore(cellId);
    }

    TFuture<void> DoSyncWithCore(TCellId cellId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto channel = CellDirectory_->FindChannel(cellId, EPeerKind::Leader);
        if (!channel) {
            return MakeFuture(TError(
                NRpc::EErrorCode::Unavailable,
                "Cannot synchronize with cell %v since it is not connected",
                cellId));
        }

        YT_LOG_DEBUG("Synchronizing with another instance (SrcCellId: %v, DstCellId: %v)",
            cellId,
            SelfCellId_);

        THiveServiceProxy proxy(std::move(channel));
        auto req = proxy.Ping();
        req->SetTimeout(Config_->PingRpcTimeout);
        ToProto(req->mutable_src_cell_id(), SelfCellId_);

        return req->Invoke()
            .Apply(
                BIND(&TImpl::OnSyncPingResponse, MakeStrong(this), cellId)
                    .AsyncVia(GuardedAutomatonInvoker_))
            // NB: Many subscribers are typically waiting for the sync to complete.
            // Make sure the promise is set in a large thread pool.
            .Apply(
                 BIND([] (const TError& error) { error.ThrowOnError(); })
                    .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
    }

    TFuture<void> OnSyncPingResponse(TCellId cellId, const THiveServiceProxy::TErrorOrRspPingPtr& rspOrError)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (!rspOrError.IsOK()) {
            THROW_ERROR_EXCEPTION(
                NRpc::EErrorCode::Unavailable,
                "Failed to synchronize with cell %v",
                cellId)
                << rspOrError;
        }

        auto* mailbox = GetMailboxOrThrow(cellId);
        if (!mailbox->GetConnected()) {
            THROW_ERROR_EXCEPTION(
                NRpc::EErrorCode::Unavailable,
                "Unable to synchronize with cell %v since it is not connected",
                cellId);
        }

        const auto& rsp = rspOrError.Value();
        if (!rsp->has_last_outcoming_message_id()) {
            YT_LOG_DEBUG("Remote instance has no mailbox; no synchronization needed (SrcCellId: %v, DstCellId: %v)",
                cellId,
                SelfCellId_);
            return VoidFuture;
        }

        auto messageId = rsp->last_outcoming_message_id();
        if (messageId < mailbox->GetNextIncomingMessageId()) {
            YT_LOG_DEBUG("Already synchronized with remote instance (SrcCellId: %v, DstCellId: %v, "
                "SyncMessageId: %v, NextPersistentIncomingMessageId: %v)",
                cellId,
                SelfCellId_,
                messageId,
                mailbox->GetNextIncomingMessageId());
            return VoidFuture;
        }

        YT_LOG_DEBUG("Waiting for synchronization with remote instance (SrcCellId: %v, DstCellId: %v, "
            "SyncMessageId: %v, NextPersistentIncomingMessageId: %v)",
            cellId,
            SelfCellId_,
            messageId,
            mailbox->GetNextIncomingMessageId());

        return RegisterSyncRequest(mailbox, messageId);
    }

    TFuture<void> RegisterSyncRequest(TMailbox* mailbox, TMessageId messageId)
    {
        auto& syncRequests = mailbox->SyncRequests();

        auto it = syncRequests.find(messageId);
        if (it != syncRequests.end()) {
            return it->second.ToFuture();
        }

        auto promise = NewPromise<void>();
        YT_VERIFY(syncRequests.emplace(messageId, promise).second);
        return promise.ToFuture();
    }

    void FlushSyncRequests(TMailbox* mailbox)
    {
        auto& syncRequests = mailbox->SyncRequests();
        while (!syncRequests.empty()) {
            auto it = syncRequests.begin();
            auto messageId = it->first;
            if (messageId >= mailbox->GetNextIncomingMessageId()) {
                break;
            }

            YT_LOG_DEBUG("Synchronization complete (SrcCellId: %v, DstCellId: %v, MessageId: %v)",
                SelfCellId_,
                mailbox->GetCellId(),
                messageId);

            it->second.Set();
            syncRequests.erase(it);
        }
    }

    void OnIdlePostOutcomingMessages(TCellId cellId)
    {
        TCounterIncrementingTimingGuard<TWallTimer> timingGuard(Profiler, &PostingTimeCounter_);

        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        PostOutcomingMessages(mailbox, true);
    }

    void SchedulePostOutcomingMessages(TMailbox* mailbox)
    {
        if (mailbox->GetPostBatchingCookie()) {
            return;
        }

        mailbox->SetPostBatchingCookie(TDelayedExecutor::Submit(
            BIND([this, this_ = MakeStrong(this), cellId = mailbox->GetCellId()] {
                TCounterIncrementingTimingGuard<TWallTimer> timingGuard(Profiler, &PostingTimeCounter_);

                auto* mailbox = FindMailbox(cellId);
                if (!mailbox) {
                    return;
                }

                mailbox->SetPostBatchingCookie(nullptr);
                PostOutcomingMessages(mailbox, false);
            }).Via(EpochAutomatonInvoker_),
            Config_->PostBatchingPeriod));
    }

    void PostOutcomingMessages(TMailbox* mailbox, bool allowIdle)
    {
        if (!IsLeader()) {
            return;
        }

        if (!mailbox->GetConnected()) {
            return;
        }

        if (mailbox->GetInFlightOutcomingMessageCount() > 0) {
            return;
        }

        auto firstMessageId = mailbox->GetFirstInFlightOutcomingMessageId();
        const auto& outcomingMessages = mailbox->OutcomingMessages();
        YT_VERIFY(firstMessageId >= mailbox->GetFirstOutcomingMessageId());
        YT_VERIFY(firstMessageId <= mailbox->GetFirstOutcomingMessageId() + outcomingMessages.size());

        TDelayedExecutor::CancelAndClear(mailbox->IdlePostCookie());
        if (!allowIdle && firstMessageId == mailbox->GetFirstOutcomingMessageId() + outcomingMessages.size()) {
            mailbox->IdlePostCookie() = TDelayedExecutor::Submit(
                BIND(&TImpl::OnIdlePostOutcomingMessages, MakeWeak(this), mailbox->GetCellId())
                    .Via(EpochAutomatonInvoker_),
                Config_->IdlePostPeriod);
            return;
        }

        auto channel = FindMailboxChannel(mailbox);
        if (!channel) {
            return;
        }

        THiveServiceProxy proxy(std::move(channel));
        auto req = proxy.PostMessages();
        req->SetTimeout(Config_->PostRpcTimeout);
        ToProto(req->mutable_src_cell_id(), SelfCellId_);
        req->set_first_message_id(firstMessageId);

        int messagesToPost = 0;
        i64 bytesToPost = 0;
        while (firstMessageId + messagesToPost < mailbox->GetFirstOutcomingMessageId() + outcomingMessages.size() &&
               messagesToPost < Config_->MaxMessagesPerPost &&
               bytesToPost < Config_->MaxBytesPerPost)
        {
            const auto& message = outcomingMessages[firstMessageId + messagesToPost - mailbox->GetFirstOutcomingMessageId()];
            req->add_messages()->CopyFrom(*message);
            messagesToPost += 1;
            bytesToPost += message->ByteSize();
        }

        mailbox->SetInFlightOutcomingMessageCount(messagesToPost);
        mailbox->SetPostInProgress(true);

        if (messagesToPost == 0) {
            YT_LOG_DEBUG("Checking mailbox synchronization (SrcCellId: %v, DstCellId: %v)",
                SelfCellId_,
                mailbox->GetCellId());
        } else {
            YT_LOG_DEBUG("Posting reliable outcoming messages (SrcCellId: %v, DstCellId: %v, MessageIds: %v-%v)",
                SelfCellId_,
                mailbox->GetCellId(),
                firstMessageId,
                firstMessageId + messagesToPost - 1);
        }

        req->Invoke().Subscribe(
            BIND(&TImpl::OnPostMessagesResponse, MakeStrong(this), mailbox->GetCellId())
                .Via(EpochAutomatonInvoker_));
    }

    void OnPostMessagesResponse(TCellId cellId, const THiveServiceProxy::TErrorOrRspPostMessagesPtr& rspOrError)
    {
        TCounterIncrementingTimingGuard<TWallTimer> timingGuard(Profiler, &PostingTimeCounter_);

        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        if (!mailbox->GetPostInProgress()) {
            return;
        }

        mailbox->SetInFlightOutcomingMessageCount(0);
        mailbox->SetPostInProgress(false);

        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Failed to post reliable outcoming messages (SrcCellId: %v, DstCellId: %v)",
                SelfCellId_,
                mailbox->GetCellId());
            SetMailboxDisconnected(mailbox);
            return;
        }

        const auto& rsp = rspOrError.Value();
        auto nextPersistentIncomingMessageId = rsp->has_next_persistent_incoming_message_id()
            ? std::make_optional(rsp->next_persistent_incoming_message_id())
            : std::nullopt;
        auto nextTransientIncomingMessageId = rsp->next_transient_incoming_message_id();
        YT_LOG_DEBUG("Outcoming reliable messages posted (SrcCellId: %v, DstCellId: %v, "
            "NextPersistentIncomingMessageId: %v, NextTransientIncomingMessageId: %v)",
            SelfCellId_,
            mailbox->GetCellId(),
            nextPersistentIncomingMessageId,
            nextTransientIncomingMessageId);

        if (nextPersistentIncomingMessageId && !HandlePersistentIncomingMessages(mailbox, *nextPersistentIncomingMessageId)) {
            return;
        }

        if (!HandleTransientIncomingMessages(mailbox, nextTransientIncomingMessageId)) {
            return;
        }

        SchedulePostOutcomingMessages(mailbox);
    }

    void OnSendMessagesResponse(TCellId cellId, const THiveServiceProxy::TErrorOrRspSendMessagesPtr& rspOrError)
    {
        TCounterIncrementingTimingGuard<TWallTimer> timingGuard(Profiler, &PostingTimeCounter_);

        auto* mailbox = FindMailbox(cellId);
        if (!mailbox) {
            return;
        }

        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Failed to send unreliable outcoming messages (SrcCellId: %v, DstCellId: %v)",
                SelfCellId_,
                mailbox->GetCellId());
            SetMailboxDisconnected(mailbox);
            return;
        }

        YT_LOG_DEBUG("Outcoming unreliable messages sent successfully (SrcCellId: %v, DstCellId: %v)",
            SelfCellId_,
            mailbox->GetCellId());
    }


    std::unique_ptr<TMutation> CreateAcknowledgeMessagesMutation(const NHiveServer::NProto::TReqAcknowledgeMessages& req)
    {
        return CreateMutation(
            HydraManager_,
            req,
            &TImpl::HydraAcknowledgeMessages,
            this);
    }

    std::unique_ptr<TMutation> CreatePostMessagesMutation(const NHiveClient::NProto::TReqPostMessages& request)
    {
        return CreateMutation(
            HydraManager_,
            request,
            &TImpl::HydraPostMessages,
            this);
    }

    std::unique_ptr<TMutation> CreateSendMessagesMutation(const TCtxSendMessagesPtr& context)
    {
        return CreateMutation(
            HydraManager_,
            context,
            &TImpl::HydraSendMessages,
            this);
    }

    std::unique_ptr<TMutation> CreateUnregisterMailboxMutation(const NHiveServer::NProto::TReqUnregisterMailbox& req)
    {
        return CreateMutation(
            HydraManager_,
            req,
            &TImpl::HydraUnregisterMailbox,
            this);
    }


    bool CheckRequestedMessageIdAgainstMailbox(TMailbox* mailbox, TMessageId requestedMessageId)
    {
        if (requestedMessageId < mailbox->GetFirstOutcomingMessageId()) {
            YT_LOG_ERROR_UNLESS(IsRecovery(), "Destination is out of sync: requested to receive already truncated messages (SrcCellId: %v, DstCellId: %v, "
                "RequestedMessageId: %v, FirstOutcomingMessageId: %v)",
                SelfCellId_,
                mailbox->GetCellId(),
                requestedMessageId,
                mailbox->GetFirstOutcomingMessageId());
            SetMailboxDisconnected(mailbox);
            return false;
        }

        if (requestedMessageId > mailbox->GetFirstOutcomingMessageId() + mailbox->OutcomingMessages().size()) {
            YT_LOG_ERROR_UNLESS(IsRecovery(), "Destination is out of sync: requested to receive nonexisting messages (SrcCellId: %v, DstCellId: %v, "
                "RequestedMessageId: %v, FirstOutcomingMessageId: %v, OutcomingMessageCount: %v)",
                SelfCellId_,
                mailbox->GetCellId(),
                requestedMessageId,
                mailbox->GetFirstOutcomingMessageId(),
                mailbox->OutcomingMessages().size());
            SetMailboxDisconnected(mailbox);
            return false;
        }

        return true;
    }

    bool HandlePersistentIncomingMessages(TMailbox* mailbox, TMessageId nextPersistentIncomingMessageId)
    {
        if (!CheckRequestedMessageIdAgainstMailbox(mailbox, nextPersistentIncomingMessageId)) {
            return false;
        }

        if (mailbox->GetAcknowledgeInProgress()) {
            return true;
        }

        if (nextPersistentIncomingMessageId == mailbox->GetFirstOutcomingMessageId()) {
            return true;
        }

        NHiveServer::NProto::TReqAcknowledgeMessages req;
        ToProto(req.mutable_cell_id(), mailbox->GetCellId());
        req.set_next_persistent_incoming_message_id(nextPersistentIncomingMessageId);

        mailbox->SetAcknowledgeInProgress(true);

        YT_LOG_DEBUG_UNLESS(IsRecovery(), "Committing reliable messages acknowledgement (SrcCellId: %v, DstCellId: %v, "
            "MessageIds: %v-%v)",
            SelfCellId_,
            mailbox->GetCellId(),
            mailbox->GetFirstOutcomingMessageId(),
            nextPersistentIncomingMessageId - 1);

        CreateAcknowledgeMessagesMutation(req)
            ->CommitAndLog(Logger);

        return true;
    }

    bool HandleTransientIncomingMessages(TMailbox* mailbox, TMessageId nextTransientIncomingMessageId)
    {
        if (!CheckRequestedMessageIdAgainstMailbox(mailbox, nextTransientIncomingMessageId)) {
            return false;
        }

        mailbox->SetFirstInFlightOutcomingMessageId(nextTransientIncomingMessageId);
        return true;
    }


    void ApplyReliableIncomingMessages(TMailbox* mailbox, const NHiveClient::NProto::TReqPostMessages* req)
    {
        for (int index = 0; index < req->messages_size(); ++index) {
            auto messageId = req->first_message_id() + index;
            ApplyReliableIncomingMessage(mailbox, messageId, req->messages(index));
        }
    }

    void ApplyReliableIncomingMessage(TMailbox* mailbox, TMessageId messageId, const TEncapsulatedMessage& message)
    {
        if (messageId != mailbox->GetNextIncomingMessageId()) {
            YT_LOG_ERROR_UNLESS(IsRecovery(), "Unexpected error: attempt to apply an out-of-order message (SrcCellId: %v, DstCellId: %v, "
                "ExpectedMessageId: %v, ActualMessageId: %v, MutationType: %v)",
                mailbox->GetCellId(),
                SelfCellId_,
                mailbox->GetNextIncomingMessageId(),
                messageId,
                message.type());
            return;
        }

        YT_LOG_DEBUG_UNLESS(IsRecovery(), "Applying reliable incoming message (SrcCellId: %v, DstCellId: %v, MessageId: %v, MutationType: %v)",
            mailbox->GetCellId(),
            SelfCellId_,
            messageId,
            message.type());

        ApplyMessage(message);

        mailbox->SetNextIncomingMessageId(messageId + 1);

        FlushSyncRequests(mailbox);
    }

    void ApplyUnreliableIncomingMessages(TMailbox* mailbox, const NHiveClient::NProto::TReqSendMessages* req)
    {
        for (const auto& message : req->messages()) {
            ApplyUnreliableIncomingMessage(mailbox, message);
        }
    }

    void ApplyUnreliableIncomingMessage(TMailbox* mailbox, const TEncapsulatedMessage& message)
    {
        YT_LOG_DEBUG_UNLESS(IsRecovery(), "Applying unreliable incoming message (SrcCellId: %v, DstCellId: %v, MutationType: %v)",
            mailbox->GetCellId(),
            SelfCellId_,
            message.type());
        ApplyMessage(message);
    }

    void ApplyMessage(const TEncapsulatedMessage& message)
    {
        auto reign = GetCurrentMutationContext()->Request().Reign;
        auto request = TMutationRequest(reign);
        request.Type = message.type();
        request.Data = TSharedRef::FromString(message.data());

        TTraceContextGuard traceContextGuard(GetTraceContext(message));

        {
            TMutationContext mutationContext(GetCurrentMutationContext(), request);
            TMutationContextGuard mutationContextGuard(&mutationContext);

            THiveMutationGuard hiveMutationGuard;

            static_cast<IAutomaton*>(Automaton_)->ApplyMutation(&mutationContext);
        }
    }


    static TTraceContextPtr GetTraceContext(const TEncapsulatedMessage& message)
    {
        TTraceId traceId = InvalidTraceId;
        if (message.has_trace_id_old()) {
            traceId.Parts64[0] = message.trace_id_old();
        }
        if (message.has_trace_id()) {
            traceId = FromProto<TTraceId>(message.trace_id());
        }

        auto sourceSpan = InvalidSpanId;
        if (message.has_span_id()) {
            sourceSpan = message.span_id();
        }

        if (traceId == InvalidTraceId || sourceSpan == InvalidSpanId) {
            return nullptr;
        }

        auto spanName = "HiveMessage." + message.type();
        return New<TTraceContext>(
            TFollowsFrom{},
            TSpanContext{traceId, sourceSpan, message.is_sampled(), message.is_debug()},
            spanName);
    }

    static void AnnotateWithTraceContext(TEncapsulatedMessage* message)
    {
        auto traceContext = GetCurrentTraceContext();
        if (!traceContext) {
            return;
        }

        auto traceId = traceContext->GetTraceId();
        ToProto(message->mutable_trace_id(), traceId);
        message->set_span_id(traceContext->GetSpanId());

        // COMPAT(prime)
        message->set_trace_id_old(traceId.Parts64[0]);
    }


    // NB: Leader must wait until it is active before reconnecting mailboxes
    // since no commits are possible before this point.
    virtual void OnLeaderActive() override
    {
        TCompositeAutomatonPart::OnLeaderRecoveryComplete();
        ReconnectMailboxes();
    }

    virtual void OnStopLeading() override
    {
        TCompositeAutomatonPart::OnStopLeading();
        ResetMailboxes();
    }

    virtual void OnFollowerRecoveryComplete() override
    {
        TCompositeAutomatonPart::OnFollowerRecoveryComplete();
        ReconnectMailboxes();
    }

    virtual void OnStopFollowing() override
    {
        TCompositeAutomatonPart::OnStopFollowing();
        ResetMailboxes();
    }


    virtual bool ValidateSnapshotVersion(int version) override
    {
        return version == 3;
    }

    virtual int GetCurrentSnapshotVersion() override
    {
        return 3;
    }


    virtual void Clear() override
    {
        TCompositeAutomatonPart::Clear();

        MailboxMap_.Clear();
    }

    void SaveKeys(TSaveContext& context) const
    {
        MailboxMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        MailboxMap_.SaveValues(context);
    }

    void LoadKeys(TLoadContext& context)
    {
        MailboxMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        MailboxMap_.LoadValues(context);
    }


    void SyncWithUpstreamOnIncomingMessage(TCellId srcCellId)
    {
        auto handlers =  IncomingMessageUpstreamSync_.ToVector();
        if (handlers.empty()) {
            return;
        }

        std::vector<TFuture<void>> asyncResults;
        for (const auto& handler : handlers) {
            asyncResults.push_back(handler.Run(srcCellId));
        }

        auto result = WaitFor(Combine(asyncResults));
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error synchronizing with upstream upon receiving message from cell %v",
            srcCellId);
    }


    // THydraServiceBase overrides.
    virtual IHydraManagerPtr GetHydraManager() override
    {
        return HydraManager_;
    }


    IYPathServicePtr CreateOrchidService()
    {
        auto invoker = HydraManager_->CreateGuardedAutomatonInvoker(AutomatonInvoker_);
        auto producer = BIND(&TImpl::BuildOrchidYson, MakeWeak(this));
        return IYPathService::FromProducer(producer, TDuration::Seconds(1))
            ->Via(invoker);
    }

    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("mailboxes").DoMapFor(MailboxMap_, [&] (TFluentMap fluent, const std::pair<TCellId, TMailbox*>& pair) {
                    auto* mailbox = pair.second;
                    fluent
                        .Item(ToString(mailbox->GetCellId())).BeginMap()
                            .Item("connected").Value(mailbox->GetConnected())
                            .Item("acknowledge_in_progress").Value(mailbox->GetAcknowledgeInProgress())
                            .Item("post_in_progress").Value(mailbox->GetPostInProgress())
                            .Item("first_outcoming_message_id").Value(mailbox->GetFirstOutcomingMessageId())
                            .Item("outcoming_message_count").Value(mailbox->OutcomingMessages().size())
                            .Item("next_persistent_incoming_message_id").Value(mailbox->GetNextIncomingMessageId())
                            .Item("next_transient_incoming_message_id").Value(GetNextTransientIncomingMessageId(mailbox))
                            .Item("first_in_flight_outcoming_message_id").Value(mailbox->GetFirstInFlightOutcomingMessageId())
                            .Item("in_flight_outcoming_message_count").Value(mailbox->GetInFlightOutcomingMessageCount())
                        .EndMap();
                })
            .EndMap();
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(THiveManager::TImpl, Mailbox, TMailbox, MailboxMap_)

////////////////////////////////////////////////////////////////////////////////

THiveManager::THiveManager(
    THiveManagerConfigPtr config,
    TCellDirectoryPtr cellDirectory,
    TCellId selfCellId,
    IInvokerPtr automatonInvoker,
    IHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton)
    : Impl_(New<TImpl>(
        config,
        cellDirectory,
        selfCellId,
        automatonInvoker,
        hydraManager,
        automaton))
{ }

THiveManager::~THiveManager() = default;

IServicePtr THiveManager::GetRpcService()
{
    return Impl_->GetRpcService();
}

IYPathServicePtr THiveManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

TCellId THiveManager::GetSelfCellId() const
{
    return Impl_->GetSelfCellId();
}

TMailbox* THiveManager::CreateMailbox(TCellId cellId)
{
    return Impl_->CreateMailbox(cellId);
}

TMailbox* THiveManager::GetOrCreateMailbox(TCellId cellId)
{
    return Impl_->GetOrCreateMailbox(cellId);
}

TMailbox* THiveManager::GetMailboxOrThrow(TCellId cellId)
{
    return Impl_->GetMailboxOrThrow(cellId);
}

void THiveManager::RemoveMailbox(TMailbox* mailbox)
{
    Impl_->RemoveMailbox(mailbox);
}

void THiveManager::PostMessage(TMailbox* mailbox, TRefCountedEncapsulatedMessagePtr message, bool reliable)
{
    Impl_->PostMessage(mailbox, std::move(message), reliable);
}

void THiveManager::PostMessage(const TMailboxList& mailboxes, TRefCountedEncapsulatedMessagePtr message, bool reliable)
{
    Impl_->PostMessage(mailboxes, std::move(message), reliable);
}

void THiveManager::PostMessage(TMailbox* mailbox, const ::google::protobuf::MessageLite& message, bool reliable)
{
    Impl_->PostMessage(mailbox, message, reliable);
}

void THiveManager::PostMessage(const TMailboxList& mailboxes, const ::google::protobuf::MessageLite& message, bool reliable)
{
    Impl_->PostMessage(mailboxes, message, reliable);
}

TFuture<void> THiveManager::SyncWith(TCellId cellId, bool enableBatching)
{
    return Impl_->SyncWith(cellId, enableBatching);
}

DELEGATE_SIGNAL(THiveManager, TFuture<void>(NHiveClient::TCellId srcCellId), IncomingMessageUpstreamSync, *Impl_);
DELEGATE_ENTITY_MAP_ACCESSORS(THiveManager, Mailbox, TMailbox, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
