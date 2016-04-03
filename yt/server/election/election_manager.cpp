#include "election_manager.h"
#include "private.h"
#include "config.h"

#include <yt/ytlib/election/cell_manager.h>
#include <yt/ytlib/election/election_service_proxy.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/common.h>

#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NElection {

using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TElectionManager::TImpl
    : public NRpc::TServiceBase
{
public:
    TImpl(
        TElectionManagerConfigPtr config,
        TCellManagerPtr cellManager,
        IInvokerPtr controlInvoker,
        IElectionCallbacksPtr electionCallbacks);

    void Start();
    void Stop();

    NRpc::IServicePtr GetRpcService();

    TYsonProducer GetMonitoringProducer();

    TEpochContextPtr GetEpochContext();

private:
    class TVotingRound;

    class TFollowerPinger;
    typedef TIntrusivePtr<TFollowerPinger> TFollowerPingerPtr;

    const TElectionManagerConfigPtr Config;
    const TCellManagerPtr CellManager;
    const IInvokerPtr ControlInvoker;
    const IElectionCallbacksPtr ElectionCallbacks;

    EPeerState State = EPeerState::Stopped;

    // Voting parameters.
    TPeerId VoteId = InvalidPeerId;
    TEpochId VoteEpochId;

    // Epoch parameters.
    TEpochContextPtr EpochContext;
    IInvokerPtr ControlEpochInvoker;

    yhash_set<TPeerId> AliveFollowers;
    yhash_set<TPeerId> PotentialFollowers;

    NConcurrency::TDelayedExecutorCookie PingTimeoutCookie;
    TFollowerPingerPtr FollowerPinger;


    // Corresponds to #ControlInvoker.
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    DECLARE_RPC_SERVICE_METHOD(NElection::NProto, PingFollower);
    DECLARE_RPC_SERVICE_METHOD(NElection::NProto, GetStatus);

    void Reset();

    void OnFollowerPingTimeout();

    void DoStart();
    void DoStop();

    bool CheckQuorum();

    void StartVotingRound();
    void StartVoteFor(TPeerId voteId, const TEpochId& voteEpochId);
    void StartVoteForSelf();

    void StartLeading();
    void StartFollowing(TPeerId leaderId, const TEpochId& epoch);
    void StopLeading();
    void StopFollowing();

    void InitEpochContext(TPeerId leaderId, const TEpochId& epoch);
    void SetState(EPeerState newState);

    void OnPeerReconfigured(TPeerId peerId);

};

////////////////////////////////////////////////////////////////////////////////

class TElectionManager::TImpl::TFollowerPinger
    : public TRefCounted
{
public:
    explicit TFollowerPinger(TImplPtr owner)
        : Owner(owner)
        , Logger(Owner->Logger)
    { }

    void Run()
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);

        const auto& cellManager = Owner->CellManager;
        for (TPeerId id = 0; id < cellManager->GetPeerCount(); ++id) {
            if (id != cellManager->GetSelfPeerId()) {
                SendPing(id);
            }
        }
    }

private:
    const TImplPtr Owner;
    const NLogging::TLogger Logger;


    void SendPing(TPeerId peerId)
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);

        auto channel = Owner->CellManager->GetPeerChannel(peerId);
        if (!channel) {
            SchedulePing(peerId);
            return;
        }

        LOG_DEBUG("Sending ping to follower %v", peerId);

        TElectionServiceProxy proxy(channel);
        auto req = proxy.PingFollower();
        req->SetTimeout(Owner->Config->FollowerPingRpcTimeout);
        req->set_leader_id(Owner->CellManager->GetSelfPeerId());
        ToProto(req->mutable_epoch_id(), Owner->EpochContext->EpochId);

        req->Invoke().Subscribe(
            BIND(&TFollowerPinger::OnPingResponse, MakeStrong(this), peerId)
                .Via(Owner->ControlEpochInvoker));
    }

    void SchedulePing(TPeerId id)
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);

        TDelayedExecutor::Submit(
            BIND(&TFollowerPinger::SendPing, MakeStrong(this), id)
                .Via(Owner->ControlEpochInvoker),
            Owner->Config->FollowerPingPeriod);
    }

    void OnPingResponse(TPeerId id, const TElectionServiceProxy::TErrorOrRspPingFollowerPtr& rspOrError)
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);
        YCHECK(Owner->State == EPeerState::Leading);

        if (rspOrError.IsOK()) {
            OnPingResponseSuccess(id, rspOrError.Value());
        } else {
            OnPingResponseFailure(id, rspOrError);
        }
    }

    void OnPingResponseSuccess(TPeerId id, TElectionServiceProxy::TRspPingFollowerPtr rsp)
    {
        LOG_DEBUG("Ping reply from follower %v", id);

        if (Owner->PotentialFollowers.find(id) != Owner->PotentialFollowers.end()) {
            LOG_INFO("Follower %v is up, first success", id);
            YCHECK(Owner->PotentialFollowers.erase(id) == 1);
        } else if (Owner->AliveFollowers.find(id) == Owner->AliveFollowers.end()) {
            LOG_INFO("Follower %v is up", id);
            YCHECK(Owner->AliveFollowers.insert(id).second);
        }

        SchedulePing(id);
    }

    void OnPingResponseFailure(TPeerId id, const TError& error)
    {
        auto code = error.GetCode();
        if (code == NElection::EErrorCode::InvalidState ||
            code == NElection::EErrorCode::InvalidLeader ||
            code == NElection::EErrorCode::InvalidEpoch)
        {
            // These errors are possible during grace period.
            if (Owner->PotentialFollowers.find(id) == Owner->PotentialFollowers.end()) {
                if (Owner->AliveFollowers.erase(id) > 0) {
                    LOG_WARNING(error, "Error pinging follower %v, considered down",
                        id);
                }
            } else {
                if (TInstant::Now() > Owner->EpochContext->StartTime + Owner->Config->FollowerGraceTimeout) {
                    LOG_WARNING(error, "Error pinging follower %v, no success within grace period, considered down",
                        id);
                    Owner->PotentialFollowers.erase(id);
                    Owner->AliveFollowers.erase(id);
                } else {
                    LOG_INFO(error, "Error pinging follower %v, will retry later",
                        id);
                }
            }
        } else {
            if (Owner->AliveFollowers.erase(id) > 0) {
                LOG_WARNING(error, "Error pinging follower %v, considered down",
                    id);
                Owner->PotentialFollowers.erase(id);
            }
        }

        if (!Owner->CheckQuorum())
            return;

        if (code == NYT::EErrorCode::Timeout) {
            SendPing(id);
        } else {
            SchedulePing(id);
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TElectionManager::TImpl::TVotingRound
    : public TRefCounted
{
public:
    explicit TVotingRound(TImplPtr owner)
        : Owner(owner)
    {
        Logger = Owner->Logger;
        Logger.AddTag("RoundId: %v, VoteEpochId: %v",
            TGuid::Create(),
            Owner->VoteEpochId);
    }

    void Run()
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);
        YCHECK(Owner->State == EPeerState::Voting);

        auto callbacks = Owner->ElectionCallbacks;
        auto cellManager = Owner->CellManager;
        auto priority = callbacks->GetPriority();

        LOG_DEBUG("New voting round started (VoteId: %v, Priority: %v)",
            Owner->VoteId,
            callbacks->FormatPriority(priority));

        ProcessVote(
            cellManager->GetSelfPeerId(),
            TStatus(
                Owner->State,
                Owner->VoteId,
                priority,
                Owner->VoteEpochId));

        std::vector<TFuture<void>> asyncResults;
        for (TPeerId id = 0; id < cellManager->GetPeerCount(); ++id) {
            if (id == cellManager->GetSelfPeerId())
                continue;

            auto channel = Owner->CellManager->GetPeerChannel(id);
            if (!channel)
                continue;

            TElectionServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(Owner->Config->ControlRpcTimeout);

            auto req = proxy.GetStatus();
            asyncResults.push_back(
                req->Invoke().Apply(
                    BIND(&TVotingRound::OnResponse, MakeStrong(this), id)
                        .AsyncVia(Owner->ControlEpochInvoker)));
        }

        Combine(asyncResults).Subscribe(
            BIND(&TVotingRound::OnComplete, MakeStrong(this))
                .Via(Owner->ControlEpochInvoker));
    }

private:
    const TImplPtr Owner;

    struct TStatus
    {
        EPeerState State;
        TPeerId VoteId;
        TPeerPriority Priority;
        TEpochId VoteEpochId;

        TStatus(
            EPeerState state = EPeerState::Stopped,
            TPeerId vote = InvalidPeerId,
            TPeerPriority priority = -1,
            const TEpochId& voteEpochId = TEpochId())
            : State(state)
            , VoteId(vote)
            , Priority(priority)
            , VoteEpochId(voteEpochId)
        { }
    };

    typedef yhash_map<TPeerId, TStatus> TStatusTable;

    TStatusTable StatusTable;

    bool Finished = false;

    NLogging::TLogger Logger;


    void ProcessVote(TPeerId id, const TStatus& status)
    {
        YCHECK(id != InvalidPeerId);
        StatusTable[id] = status;

        for (const auto& pair : StatusTable) {
            if (CheckForLeader(pair.first, pair.second)) {
                break;
            }
        }
    }

    void OnResponse(TPeerId id, const TElectionServiceProxy::TErrorOrRspGetStatusPtr& rspOrError)
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);

        if (Finished)
            return;

        if (!rspOrError.IsOK()) {
            LOG_INFO(rspOrError, "Error requesting status from peer %v",
                id);
            return;
        }

        const auto& rsp = rspOrError.Value();
        auto state = EPeerState(rsp->state());
        auto vote = rsp->vote_id();
        auto priority = rsp->priority();
        auto epochId = FromProto<TEpochId>(rsp->vote_epoch_id());

        LOG_DEBUG("Received status from peer %v (State: %v, VoteId: %v, Priority: %v)",
            id,
            state,
            vote,
            Owner->ElectionCallbacks->FormatPriority(priority));

        ProcessVote(id, TStatus(state, vote, priority, epochId));
    }

    bool CheckForLeader(TPeerId candidateId, const TStatus& candidateStatus)
    {
        if (!IsFeasibleLeader(candidateId, candidateStatus)) {
            return false;
        }

        // Compute candidate epoch.
        // Use the local one for self
        // (others may still be following with an outdated epoch).
        auto candidateEpochId =
            candidateId == Owner->CellManager->GetSelfPeerId()
            ? Owner->VoteEpochId
            : candidateStatus.VoteEpochId;

        // Count votes (including self) and quorum.
        int voteCount = CountVotesFor(candidateId, candidateEpochId);
        int quorum = Owner->CellManager->GetQuorumCount();

        // Check for quorum.
        if (voteCount < quorum) {
            return false;
        }

        LOG_DEBUG("Candidate %v has quorum: %v >= %v",
            candidateId,
            voteCount,
            quorum);

        Finished = true;

        // Become a leader or a follower.
        if (candidateId == Owner->CellManager->GetSelfPeerId()) {
            Owner->ControlEpochInvoker->Invoke(BIND(
                &TImpl::StartLeading,
                Owner));
        } else {
            Owner->ControlEpochInvoker->Invoke(BIND(
                &TImpl::StartFollowing,
                Owner,
                candidateId,
                candidateStatus.VoteEpochId));
        }

        return true;
    }

    int CountVotesFor(TPeerId candidateId, const TEpochId& epochId) const
    {
        int result = 0;
        for (const auto& pair : StatusTable) {
            if (pair.second.VoteId == candidateId && pair.second.VoteEpochId == epochId) {
                ++result;
            }
        }
        return result;
    }

    bool IsFeasibleLeader(TPeerId candidateId, const TStatus& candidateStatus) const
    {
        // He must be voting for himself.
        if (candidateId != candidateStatus.VoteId) {
            return false;
        }

        if (candidateId == Owner->CellManager->GetSelfPeerId()) {
            // Check that we're voting.
            YCHECK(candidateStatus.State == EPeerState::Voting);
            return true;
        } else {
            // The candidate must be aware of his leadership.
            return candidateStatus.State == EPeerState::Leading;
        }
    }

    // Compare votes lexicographically by (priority, id).
    static bool IsBetterCandidate(const TStatus& lhs, const TStatus& rhs)
    {
        if (lhs.Priority > rhs.Priority) {
            return true;
        }

        if (lhs.Priority < rhs.Priority) {
            return false;
        }

        return lhs.VoteId < rhs.VoteId;
    }

    void OnComplete(const TError&)
    {
        VERIFY_THREAD_AFFINITY(Owner->ControlThread);

        if (Finished)
            return;

        LOG_DEBUG("Voting round completed");

        // Choose the best vote.
        TNullable<TStatus> bestCandidate;
        for (const auto& pair : StatusTable) {
            const auto& currentCandidate = pair.second;
            if (StatusTable.find(currentCandidate.VoteId) != StatusTable.end() &&
                (!bestCandidate || IsBetterCandidate(currentCandidate, *bestCandidate)))
            {
                bestCandidate = currentCandidate;
            }
        }

        if (bestCandidate) {
            // Extract the status of the best candidate.
            // His status must be present in the table by the above checks.
            const auto& candidateStatus = StatusTable[bestCandidate->VoteId];
            Owner->StartVoteFor(candidateStatus.VoteId, candidateStatus.VoteEpochId);
        } else {
            Owner->StartVoteForSelf();
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

TElectionManager::TImpl::TImpl(
    TElectionManagerConfigPtr config,
    TCellManagerPtr cellManager,
    IInvokerPtr controlInvoker,
    IElectionCallbacksPtr electionCallbacks)
    : TServiceBase(
        controlInvoker,
        NRpc::TServiceId(TElectionServiceProxy::GetServiceName(), cellManager->GetCellId()),
        ElectionLogger)
    , Config(config)
    , CellManager(cellManager)
    , ControlInvoker(controlInvoker)
    , ElectionCallbacks(electionCallbacks)
{
    YCHECK(Config);
    YCHECK(CellManager);
    YCHECK(ControlInvoker);
    YCHECK(ElectionCallbacks);
    VERIFY_INVOKER_THREAD_AFFINITY(ControlInvoker, ControlThread);

    Logger.AddTag("CellId: %v, SelfPeerId: %v",
        CellManager->GetCellId(),
        CellManager->GetSelfPeerId());

    Reset();

    RegisterMethod(RPC_SERVICE_METHOD_DESC(PingFollower));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(GetStatus));

    CellManager->SubscribePeerReconfigured(
        BIND(&TImpl::OnPeerReconfigured, MakeWeak(this))
            .Via(ControlInvoker));
}

void TElectionManager::TImpl::Start()
{
    ControlInvoker->Invoke(BIND(&TImpl::DoStart, MakeWeak(this)));
}

void TElectionManager::TImpl::Stop()
{
    ControlInvoker->Invoke(BIND(&TImpl::DoStop, MakeWeak(this)));
}

NRpc::IServicePtr TElectionManager::TImpl::GetRpcService()
{
    return this;
}

TYsonProducer TElectionManager::TImpl::GetMonitoringProducer()
{
    return BIND([=, this_ = MakeStrong(this)] (IYsonConsumer* consumer) {
        auto epochContext = EpochContext;
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("state").Value(FormatEnum(State))
                .Item("peers").BeginList()
                    .DoFor(0, CellManager->GetPeerCount(), [=] (TFluentList fluent, TPeerId id) {
                        fluent.Item().Value(CellManager->GetPeerAddress(id));
                    })
                .EndList()
                .DoIf(epochContext.operator bool(), [&] (TFluentMap fluent) {
                    fluent
                        .Item("leader_id").Value(epochContext->LeaderId)
                        .Item("epoch_id").Value(epochContext->EpochId);
                })
                .Item("vote_id").Value(VoteId)
            .EndMap();
    });
}

TEpochContextPtr TElectionManager::TImpl::GetEpochContext()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return EpochContext;
}

void TElectionManager::TImpl::Reset()
{
    // May be called from ControlThread and also from ctor.

    SetState(EPeerState::Stopped);

    VoteId = InvalidPeerId;

    if (EpochContext) {
        EpochContext->CancelableContext->Cancel();
    }
    EpochContext.Reset();

    AliveFollowers.clear();
    PotentialFollowers.clear();
    PingTimeoutCookie.Reset();
}

void TElectionManager::TImpl::OnFollowerPingTimeout()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(State == EPeerState::Following);

    LOG_INFO("No recurrent ping from leader within timeout");

    StopFollowing();
}

void TElectionManager::TImpl::DoStart()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    switch (State) {
        case EPeerState::Stopped:
            StartVoteForSelf();
            break;

        case EPeerState::Voting:
            break;

        case EPeerState::Leading:
            LOG_INFO("Leader restart forced");
            StopLeading();
            StartVoteForSelf();
            break;

        case EPeerState::Following:
            LOG_INFO("Follower restart forced");
            StopFollowing();
            StartVoteForSelf();
            break;

        default:
            YUNREACHABLE();
    }
}

void TElectionManager::TImpl::DoStop()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    switch (State) {
        case EPeerState::Stopped:
        case EPeerState::Voting:
            break;

        case EPeerState::Leading:
            StopLeading();
            break;

        case EPeerState::Following:
            StopFollowing();
            break;

        default:
            YUNREACHABLE();
    }

    Reset();
}

bool TElectionManager::TImpl::CheckQuorum()
{
    if (static_cast<int>(AliveFollowers.size()) >= CellManager->GetQuorumCount()) {
        return true;
    }

    LOG_WARNING("Quorum is lost");
    
    StopLeading();

    return false;
}

void TElectionManager::TImpl::StartVoteFor(TPeerId voteId, const TEpochId& voteEpoch)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    SetState(EPeerState::Voting);
    VoteId = voteId;
    VoteEpochId = voteEpoch;

    LOG_DEBUG("Voting for another candidate (VoteId: %v, VoteEpochId: %v)",
        VoteId,
        VoteEpochId);

    StartVotingRound();
}

void TElectionManager::TImpl::StartVoteForSelf()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    SetState(EPeerState::Voting);
    VoteId = CellManager->GetSelfPeerId();
    VoteEpochId = TGuid::Create();

    if (EpochContext) {
        EpochContext->CancelableContext->Cancel();
        EpochContext.Reset();
    }

    EpochContext = New<TEpochContext>();
    ControlEpochInvoker = EpochContext->CancelableContext->CreateInvoker(ControlInvoker);

    LOG_DEBUG("Voting for self (VoteId: %v, Priority: %v, VoteEpochId: %v)",
        VoteId,
        ElectionCallbacks->FormatPriority(ElectionCallbacks->GetPriority()),
        VoteEpochId);

    StartVotingRound();
}

void TElectionManager::TImpl::StartVotingRound()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(State == EPeerState::Voting);

    auto round = New<TVotingRound>(this);
    TDelayedExecutor::Submit(
        BIND(&TVotingRound::Run, round)
            .Via(ControlEpochInvoker),
        Config->VotingRoundPeriod);
}

void TElectionManager::TImpl::StartFollowing(
    TPeerId leaderId,
    const TEpochId& epochId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    SetState(EPeerState::Following);
    VoteId = leaderId;
    VoteEpochId = epochId;

    InitEpochContext(leaderId, epochId);

    PingTimeoutCookie = TDelayedExecutor::Submit(
        BIND(&TImpl::OnFollowerPingTimeout, MakeWeak(this))
            .Via(ControlEpochInvoker),
        Config->LeaderPingTimeout);

    LOG_INFO("Started following (LeaderId: %v, EpochId: %v)",
        EpochContext->LeaderId,
        EpochContext->EpochId);

    ElectionCallbacks->OnStartFollowing();
}

void TElectionManager::TImpl::StartLeading()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    SetState(EPeerState::Leading);
    YCHECK(VoteId == CellManager->GetSelfPeerId());

    // Initialize followers state.
    for (TPeerId i = 0; i < CellManager->GetPeerCount(); ++i) {
        AliveFollowers.insert(i);
        PotentialFollowers.insert(i);
    }

    InitEpochContext(CellManager->GetSelfPeerId(), VoteEpochId);

    // Send initial pings.
    YCHECK(!FollowerPinger);
    FollowerPinger = New<TFollowerPinger>(this);
    FollowerPinger->Run();

    LOG_INFO("Started leading (EpochId: %v)",
        EpochContext->EpochId);

    ElectionCallbacks->OnStartLeading();
}

void TElectionManager::TImpl::StopLeading()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(State == EPeerState::Leading);

    LOG_INFO("Stopped leading (EpochId: %v)",
        EpochContext->EpochId);

    ElectionCallbacks->OnStopLeading();

    YCHECK(FollowerPinger);
    FollowerPinger.Reset();

    Reset();
}

void TElectionManager::TImpl::StopFollowing()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(State == EPeerState::Following);

    LOG_INFO("Stopped following (LeaderId: %v, EpochId: %v)",
        EpochContext->LeaderId,
        EpochContext->EpochId);

    ElectionCallbacks->OnStopFollowing();

    Reset();
}

void TElectionManager::TImpl::InitEpochContext(TPeerId leaderId, const TEpochId& epochId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    EpochContext->LeaderId = leaderId;
    EpochContext->EpochId = epochId;
    EpochContext->StartTime = TInstant::Now();
}

void TElectionManager::TImpl::SetState(EPeerState newState)
{
    if (newState == State)
        return;

    // This generic message logged to simplify tracking state changes.
    LOG_INFO("State changed: %v -> %v",
        State,
        newState);
    State = newState;
}

void TElectionManager::TImpl::OnPeerReconfigured(TPeerId peerId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (peerId == CellManager->GetSelfPeerId()) {
        if (State == EPeerState::Leading || State == EPeerState::Following) {
            DoStart();
        }
    } else {
        if (State == EPeerState::Leading) {
            PotentialFollowers.erase(peerId);
            AliveFollowers.erase(peerId);
            CheckQuorum();
        } else if (State == EPeerState::Following && peerId == EpochContext->LeaderId) {
            DoStart();
        }
    }
}

DEFINE_RPC_SERVICE_METHOD(TElectionManager::TImpl, PingFollower)
{
    UNUSED(response);
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto epochId = FromProto<TEpochId>(request->epoch_id());
    auto leaderId = request->leader_id();

    context->SetRequestInfo("Epoch: %v, LeaderId: %v",
        epochId,
        leaderId);

    if (State != EPeerState::Following) {
        THROW_ERROR_EXCEPTION(
            NElection::EErrorCode::InvalidState,
            "Received ping in invalid state: expected %Qlv, actual %Qlv",
            EPeerState::Following,
            State);
    }

    if (epochId != EpochContext->EpochId) {
        THROW_ERROR_EXCEPTION(
            NElection::EErrorCode::InvalidEpoch,
            "Received ping with invalid epoch: expected %v, received %v",
            EpochContext->EpochId,
            epochId);
    }

    if (leaderId != EpochContext->LeaderId) {
        THROW_ERROR_EXCEPTION(
            NElection::EErrorCode::InvalidLeader,
            "Ping from an invalid leader: expected %v, received %v",
            EpochContext->LeaderId,
            leaderId);
    }

    TDelayedExecutor::Cancel(PingTimeoutCookie);
    PingTimeoutCookie = TDelayedExecutor::Submit(
        BIND(&TImpl::OnFollowerPingTimeout, MakeWeak(this))
            .Via(ControlEpochInvoker),
        Config->LeaderPingTimeout);

    context->Reply();
}

DEFINE_RPC_SERVICE_METHOD(TElectionManager::TImpl, GetStatus)
{
    UNUSED(request);
    VERIFY_THREAD_AFFINITY(ControlThread);

    context->SetRequestInfo();

    auto priority = ElectionCallbacks->GetPriority();

    response->set_state(static_cast<int>(State));
    response->set_vote_id(VoteId);
    response->set_priority(priority);
    ToProto(response->mutable_vote_epoch_id(), VoteEpochId);
    response->set_self_id(CellManager->GetSelfPeerId());

    context->SetResponseInfo("State: %v, VoteId: %v, Priority: %v, VoteEpochId: %v",
        State,
        VoteId,
        ~ElectionCallbacks->FormatPriority(priority),
        VoteEpochId);

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

TElectionManager::TElectionManager(
    TElectionManagerConfigPtr config,
    TCellManagerPtr cellManager,
    IInvokerPtr controlInvoker,
    IElectionCallbacksPtr electionCallbacks)
    : Impl(New<TImpl>(
        config,
        cellManager,
        controlInvoker,
        electionCallbacks))
{ }

TElectionManager::~TElectionManager()
{ }

void TElectionManager::Start()
{
    Impl->Start();
}

void TElectionManager::Stop()
{
    Impl->Stop();
}

NRpc::IServicePtr TElectionManager::GetRpcService()
{
    return Impl->GetRpcService();
}

TYsonProducer TElectionManager::GetMonitoringProducer()
{
    return Impl->GetMonitoringProducer();
}

TEpochContextPtr TElectionManager::GetEpochContext()
{
    return Impl->GetEpochContext();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
