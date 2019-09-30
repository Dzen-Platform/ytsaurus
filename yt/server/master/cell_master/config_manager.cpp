#include "automaton.h"
#include "bootstrap.h"
#include "config_manager.h"
#include "multicell_manager.h"
#include "config.h"

#include <yt/server/master/tablet_server/config.h>

#include <yt/core/misc/serialize.h>

#include <yt/core/ytree/ypath_proxy.h>

namespace NYT::NCellMaster {

using namespace NHydra;
using namespace NObjectClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TConfigManager::TImpl
    : public TMasterAutomatonPart
{
public:
    explicit TImpl(NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, NCellMaster::EAutomatonThreadQueue::ConfigManager)
    {
        RegisterLoader(
            "ConfigManager",
            BIND(&TImpl::Load, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "ConfigManager",
            BIND(&TImpl::Save, Unretained(this)));
    }

    void Initialize()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->SubscribeReplicateValuesToSecondaryMaster(
                BIND(&TImpl::OnReplicateValuesToSecondaryMaster, MakeWeak(this)));
        }
    }

    const TDynamicClusterConfigPtr& GetConfig() const
    {
        return Config_;
    }

    void SetConfig(TDynamicClusterConfigPtr config)
    {
        Config_ = std::move(config);
        ReplicateConfigToSecondaryMasters();
        ConfigChanged_.Fire();
    }

    DEFINE_SIGNAL(void(), ConfigChanged);

private:
    TDynamicClusterConfigPtr Config_ = New<TDynamicClusterConfig>();

    void Save(NCellMaster::TSaveContext& context) const
    {
        using NYT::Save;
        Save(context, *Config_);
    }

    void Load(NCellMaster::TLoadContext& context)
    {
        using NYT::Load;
        Load(context, *Config_);
    }

    void OnReplicateValuesToSecondaryMaster(TCellTag cellTag)
    {
        auto req = TYPathProxy::Set("//sys/@config");
        req->set_value(ConvertToYsonString(GetConfig()).GetData());

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        multicellManager->PostToMaster(req, cellTag);
    }

    void ReplicateConfigToSecondaryMasters()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            auto req = TYPathProxy::Set("//sys/@config");
            req->set_value(ConvertToYsonString(GetConfig()).GetData());
            multicellManager->PostToSecondaryMasters(req);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TConfigManager::TConfigManager(
    NCellMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(bootstrap))
{ }

TConfigManager::~TConfigManager() = default;

void TConfigManager::Initialize()
{
    Impl_->Initialize();
}

const TDynamicClusterConfigPtr& TConfigManager::GetConfig() const
{
    return Impl_->GetConfig();
}

void TConfigManager::SetConfig(TDynamicClusterConfigPtr config)
{
    Impl_->SetConfig(std::move(config));
}

DELEGATE_SIGNAL(TConfigManager, void(), ConfigChanged, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
