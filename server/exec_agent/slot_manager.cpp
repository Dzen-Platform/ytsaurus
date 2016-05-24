#include "slot_manager.h"
#include "private.h"
#include "config.h"
#include "slot.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/data_node/chunk_cache.h>
#include <yt/server/data_node/master_connector.h>

#ifdef _unix_
    #include <sys/stat.h>
#endif

namespace NYT {
namespace NExecAgent {

using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecAgentLogger;

static const char* CGroupPrefix = "slots";

////////////////////////////////////////////////////////////////////////////////

TSlotManager::TSlotManager(
    TSlotManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
{
    YCHECK(config);
    YCHECK(bootstrap);
}

void TSlotManager::Initialize(int slotCount)
{
    bool jobControlEnabled = false;

#ifdef _unix_
    if (Config_->EnforceJobControl) {
        uid_t ruid, euid, suid;
#ifdef _linux_
        YCHECK(getresuid(&ruid, &euid, &suid) == 0);
#else
        ruid = getuid();
        euid = geteuid();
        setuid(0);
        suid = getuid();
        YCHECK(seteuid(euid) == 0);
        YCHECK(setruid(ruid) == 0);
#endif
        if (suid != 0) {
            THROW_ERROR_EXCEPTION("Failed to initialize job control, make sure you run as root");
        }
        umask(0000);
        jobControlEnabled = true;
    }
#endif

    SlotPathCounters_.resize(Config_->Paths.size());

    try {
        auto nodeRpcPort = Bootstrap_->GetConfig()->RpcPort;

        const auto& execAgentConfig = Bootstrap_->GetConfig()->ExecAgent;
        Config_->EnableCGroups = execAgentConfig->EnableCGroups;
        Config_->SupportedCGroups = execAgentConfig->SupportedCGroups;

        for (int slotId = 0; slotId < slotCount; ++slotId) {
            auto slotName = ToString(slotId);
            std::vector<Stroka> slotPaths;
            for (const auto& path : Config_->Paths) {
                slotPaths.push_back(NFS::CombinePaths(path, slotName));
            }
            TNullable<int> userId(Null);
            if (jobControlEnabled) {
                userId = Config_->StartUid + slotId;
            }
            auto slot = New<TSlot>(
                Config_,
                std::move(slotPaths),
                Format("yt-node-%v", nodeRpcPort),
                ActionQueue_->GetInvoker(),
                slotId,
                userId);
            slot->Initialize();
            Slots_.push_back(slot);
        }

        if (Config_->EnableCGroups && Config_->IsCGroupSupported(NCGroup::TCpu::Name)) {
            auto cpuCGroup = NCGroup::TCpu(CGroupPrefix);
            cpuCGroup.EnsureExistance();
            cpuCGroup.SetShare(Config_->CGroupCpuShare);
        }
    } catch (const std::exception& ex) {
        if (Config_->SlotInitializationFailureIsFatal) {
            throw;
        }
        auto error = TError("Failed to initialize slots") << ex;
        LOG_WARNING(error);
        Bootstrap_->GetMasterConnector()->RegisterAlert(error);
        IsEnabled_ = false;
    }

    auto chunkCache = Bootstrap_->GetChunkCache();
    IsEnabled_ &= chunkCache->IsEnabled();
}

TSlotPtr TSlotManager::AcquireSlot()
{
    auto pathIndexIt = std::min_element(
        SlotPathCounters_.begin(),
        SlotPathCounters_.end());
    int pathIndex = std::distance(SlotPathCounters_.begin(), pathIndexIt);

    for (auto slot : Slots_) {
        if (slot->IsFree()) {
            ++SlotPathCounters_[pathIndex];
            slot->Acquire(pathIndex);
            return slot;
        }
    }
    YUNREACHABLE();
}

void TSlotManager::ReleaseSlot(TSlotPtr slot)
{
    auto pathIndex = slot->GetPathIndex();
    --SlotPathCounters_[pathIndex];
    slot->Release();
}

int TSlotManager::GetSlotCount() const
{
    return IsEnabled_ ? static_cast<int>(Slots_.size()) : 0;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} // namespace NExecAgent
