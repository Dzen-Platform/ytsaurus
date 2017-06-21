#include "account_proxy.h"
#include "account.h"
#include "security_manager.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/server/chunk_server/chunk_manager.h>
#include <yt/server/chunk_server/medium.h>

#include <yt/ytlib/security_client/account_ypath.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NRpc;
using namespace NObjectServer;
using namespace NChunkServer;

using ::ToString;

////////////////////////////////////////////////////////////////////////////////

class TAccountProxy
    : public TNonversionedObjectProxyBase<TAccount>
{
public:
    TAccountProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TAccount* account)
        : TBase(bootstrap, metadata, account)
    { }

private:
    typedef TNonversionedObjectProxyBase<TAccount> TBase;

    virtual void ValidateRemoval() override
    {
        const auto* account = GetThisImpl();
        if (account->IsBuiltin()) {
            THROW_ERROR_EXCEPTION("Cannot remove a built-in account %Qv",
                account->GetName());
        }
    }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back(TAttributeDescriptor("name")
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back("resource_usage");
        descriptors->push_back("committed_resource_usage");
        descriptors->push_back(TAttributeDescriptor("multicell_statistics")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("resource_limits")
            .SetReplicated(true));
        descriptors->push_back("violated_resource_limits");
    }

    virtual bool GetBuiltinAttribute(const TString& key, NYson::IYsonConsumer* consumer) override
    {
        const auto* account = GetThisImpl();

        if (key == "name") {
            BuildYsonFluently(consumer)
                .Value(account->GetName());
            return true;
        }

        if (key == "resource_usage") {
            SerializeClusterResources(account->ClusterStatistics().ResourceUsage, consumer);
            return true;
        }

        if (key == "committed_resource_usage") {
            SerializeClusterResources(account->ClusterStatistics().CommittedResourceUsage, consumer);
            return true;
        }

        if (key == "multicell_statistics") {
            const auto& chunkManager = Bootstrap_->GetChunkManager();

            BuildYsonFluently(consumer)
                .DoMapFor(account->MulticellStatistics(), [&] (TFluentMap fluent, const std::pair<TCellTag, const TAccountStatistics&>& pair) {
                    fluent.Item(ToString(pair.first));
                    Serialize(pair.second, fluent, chunkManager);
                });
            return true;
        }

        if (key == "resource_limits") {
            SerializeClusterResources(account->ClusterResourceLimits(), consumer);
            return true;
        }

        if (key == "violated_resource_limits") {
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            BuildYsonFluently(consumer)
                .BeginMap()
                    .Item("disk_space").Value(account->IsDiskSpaceLimitViolated())
                    .Item("disk_space_per_medium").DoMapFor(chunkManager->Media(),
                        [&] (TFluentMap fluent, const std::pair<const TMediumId&, TMedium*>& pair) {
                            const auto* medium = pair.second;
                            fluent
                                .Item(medium->GetName()).Value(account->IsDiskSpaceLimitViolated(medium->GetIndex()));
                        })
                    .Item("node_count").Value(account->IsNodeCountLimitViolated())
                    .Item("chunk_count").Value(account->IsChunkCountLimitViolated())
                    .Item("tablet_count").Value(account->IsTabletCountLimitViolated())
                    .Item("tablet_static_memory").Value(account->IsTabletStaticMemoryLimitViolated())
                .EndMap();
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const TString& key, const NYson::TYsonString& value) override
    {
        auto* account = GetThisImpl();
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        if (key == "resource_limits") {
            auto limits = ConvertTo<TSerializableClusterResourcesPtr>(value);
            account->ClusterResourceLimits() = limits->ToClusterResources(chunkManager);
            return true;
        }

        if (key == "name") {
            auto newName = ConvertTo<TString>(value);
            securityManager->RenameAccount(account, newName);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }

    void SerializeClusterResources(const TClusterResources& clusterResources, NYson::IYsonConsumer* consumer)
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto resourceSerializer = New<TSerializableClusterResources>(chunkManager, clusterResources);
        BuildYsonFluently(consumer)
            .Value(resourceSerializer);
    }

};

IObjectProxyPtr CreateAccountProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TAccount* account)
{
    return New<TAccountProxy>(bootstrap, metadata, account);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT

