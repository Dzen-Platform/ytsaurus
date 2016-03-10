#include "request_tracker.h"
#include "private.h"
#include "config.h"
#include "security_manager.h"
#include "user.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>

#include <yt/server/object_server/object_manager.h>

#include <yt/core/profiling/timing.h>

namespace NYT {
namespace NSecurityServer {

using namespace NConcurrency;
using namespace NHydra;

using NYT::ToProto;
using NYT::FromProto;

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
        Config_->UserStatisticsFlushPeriod);
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

void TRequestTracker::ChargeUser(
    TUser* user,
    int requestCount,
    TDuration readRequestTime,
    TDuration writeRequestTime)
{
    YCHECK(FlushExecutor_);

    int index = user->GetRequestStatisticsUpdateIndex();
    if (index < 0) {
        index = Request_.entries_size();
        user->SetRequestStatisticsUpdateIndex(index);
        UsersWithsEntry_.push_back(user);

        auto* entry = Request_.add_entries();
        ToProto(entry->mutable_user_id(), user->GetId());
    
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->WeakRefObject(user);
    }
    
    auto now = NProfiling::CpuInstantToInstant(NProfiling::GetCpuInstant());
    auto* entry = Request_.mutable_entries(index);
    auto* statistics = entry->mutable_statistics();
    statistics->set_request_count(statistics->request_count() + requestCount);
    statistics->set_read_request_time(ToProto(FromProto<TDuration>(statistics->read_request_time()) + readRequestTime));
    statistics->set_write_request_time(ToProto(FromProto<TDuration>(statistics->write_request_time()) + writeRequestTime));
    statistics->set_access_time(ToProto(now));
}

void TRequestTracker::Reset()
{
    auto objectManager = Bootstrap_->GetObjectManager();
    for (auto* user : UsersWithsEntry_) {
        user->SetRequestStatisticsUpdateIndex(-1);
        objectManager->WeakUnrefObject(user);
    }    

    Request_.Clear();
    UsersWithsEntry_.clear();
}

void TRequestTracker::OnFlush()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    if (UsersWithsEntry_.empty() ||
        !hydraManager->IsActiveLeader() && !hydraManager->IsActiveFollower())
    {
        return;
    }

    LOG_DEBUG("Starting user statistics commit for %v users",
        Request_.entries_size());

    auto hydraFacade = Bootstrap_->GetHydraFacade();
    auto asyncResult = CreateMutation(hydraFacade->GetHydraManager(), Request_)
        ->SetAllowLeaderForwarding(true)
        ->CommitAndLog(Logger);

    Reset();

    WaitFor(asyncResult);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
