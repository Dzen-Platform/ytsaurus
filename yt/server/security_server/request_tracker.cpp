#include "stdafx.h"
#include "request_tracker.h"
#include "user.h"
#include "config.h"
#include "security_manager.h"
#include "private.h"

#include <core/profiling/timing.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/hydra_facade.h>

#include <server/object_server/object_manager.h>

namespace NYT {
namespace NSecurityServer {

using namespace NConcurrency;
using namespace NHydra;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SecurityServerLogger;

////////////////////////////////////////////////////////////////////////////////

TRequestTracker::TRequestTracker(
    TSecurityManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
{ }

void TRequestTracker::Start()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    YCHECK(!FlushExecutor_);
    FlushExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(),
        BIND(&TRequestTracker::OnFlush, MakeWeak(this)),
        Config_->StatisticsFlushPeriod,
        EPeriodicExecutorMode::Manual);
    FlushExecutor_->Start();
}

void TRequestTracker::Stop()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto securityManager = Bootstrap_->GetSecurityManager();
    for (const auto& pair : securityManager->Users()) {
        auto* user = pair.second;
        user->ResetRequestRate();
    }

    FlushExecutor_.Reset();
    Reset();
}

void TRequestTracker::ChargeUser(TUser* user, int requestCount)
{
    YCHECK(FlushExecutor_);

    auto* update = user->GetRequestStatisticsUpdate();
    if (!update) {
        update = UpdateRequestStatisticsRequest_.add_updates();
        ToProto(update->mutable_user_id(), user->GetId());
    
        user->SetRequestStatisticsUpdate(update);
        UsersWithRequestStatisticsUpdate_.push_back(user);
    
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->WeakRefObject(user);
    }
    
    auto now = NProfiling::CpuInstantToInstant(NProfiling::GetCpuInstant());
    update->set_access_time(now.MicroSeconds());
    update->set_request_counter_delta(update->request_counter_delta() + requestCount);
}

void TRequestTracker::Reset()
{
    auto objectManager = Bootstrap_->GetObjectManager();
    for (auto* user : UsersWithRequestStatisticsUpdate_) {
        user->SetRequestStatisticsUpdate(nullptr);
        objectManager->WeakUnrefObject(user);
    }    

    UpdateRequestStatisticsRequest_.Clear();
    UsersWithRequestStatisticsUpdate_.clear();
}

void TRequestTracker::OnFlush()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    if (UsersWithRequestStatisticsUpdate_.empty() ||
        !hydraManager->IsActiveLeader() && !hydraManager->IsActiveFollower())
    {
        FlushExecutor_->ScheduleNext();
        return;
    }

    LOG_DEBUG("Starting request statistics commit for %v users",
        UpdateRequestStatisticsRequest_.updates_size());

    auto this_ = MakeStrong(this);
    auto invoker = Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker();
    CreateMutation(hydraManager, UpdateRequestStatisticsRequest_)
        ->SetAllowLeaderForwarding(true)
        ->Commit()
        .Subscribe(BIND([this, this_] (const TErrorOr<TMutationResponse>& result) {
            if (result.IsOK()) {
                FlushExecutor_->ScheduleOutOfBand();
            }
            FlushExecutor_->ScheduleNext();
        }).Via(invoker));

    Reset();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
