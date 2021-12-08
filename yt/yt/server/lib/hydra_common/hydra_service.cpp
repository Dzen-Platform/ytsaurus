#include "hydra_manager.h"
#include "hydra_service.h"

#include <yt/yt/server/lib/election/election_manager.h>

#include <yt/yt/ytlib/hydra/proto/hydra_service.pb.h>

#include <yt/yt/core/actions/cancelable_context.h>

namespace NYT::NHydra {

using namespace NConcurrency;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

THydraServiceBase::THydraServiceBase(
    IInvokerPtr invoker,
    const TServiceDescriptor& descriptor,
    const NLogging::TLogger& logger,
    TRealmId realmId)
    : TServiceBase(
        invoker,
        descriptor,
        logger,
        realmId)
{ }

void THydraServiceBase::ValidatePeer(EPeerKind kind)
{
    auto hydraManager = GetHydraManager();
    hydraManager->ValidatePeer(kind);

    auto cancelableInvoker = hydraManager
        ->GetAutomatonCancelableContext()
        ->CreateInvoker(GetCurrentInvoker());
    SetCurrentInvoker(std::move(cancelableInvoker));
}

void THydraServiceBase::SyncWithUpstream()
{
    WaitFor(DoSyncWithUpstream())
        .ThrowOnError();
}

TFuture<void> THydraServiceBase::DoSyncWithUpstream()
{
    return GetHydraManager()->SyncWithLeader();
}

bool THydraServiceBase::IsUp(const TCtxDiscoverPtr& context)
{
    const auto& request = context->Request();
    EPeerKind kind;
    if (request.HasExtension(NProto::TPeerKindExt::peer_kind_ext)) {
        const auto& ext = request.GetExtension(NProto::TPeerKindExt::peer_kind_ext);
        kind = CheckedEnumCast<EPeerKind>(ext.peer_kind());
    } else {
        kind = EPeerKind::Leader;
    }

    auto hydraManager = GetHydraManager();
    if (!hydraManager) {
        return false;
    }

    bool isLeader = hydraManager->IsActiveLeader();
    bool isFollower = hydraManager->IsActiveFollower();
    switch (kind) {
        case EPeerKind::Leader:
            return isLeader;
        case EPeerKind::Follower:
            return isFollower;
        case EPeerKind::LeaderOrFollower:
            return isLeader || isFollower;
        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
