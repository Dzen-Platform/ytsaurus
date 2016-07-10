#include "account_proxy.h"
#include "account.h"
#include "security_manager.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/ytlib/security_client/account_ypath.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NRpc;
using namespace NObjectServer;

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

    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        const auto* account = GetThisImpl();

        if (key == "name") {
            BuildYsonFluently(consumer)
                .Value(account->GetName());
            return true;
        }

        if (key == "resource_usage") {
            BuildYsonFluently(consumer)
                .Value(account->ClusterStatistics().ResourceUsage);
            return true;
        }

        if (key == "committed_resource_usage") {
            BuildYsonFluently(consumer)
                .Value(account->ClusterStatistics().CommittedResourceUsage);
            return true;
        }

        if (key == "multicell_statistics") {
            BuildYsonFluently(consumer)
                .DoMapFor(account->MulticellStatistics(), [] (TFluentMap fluent, const std::pair<TCellTag, const TAccountStatistics&>& pair) {
                    fluent.Item(ToString(pair.first)).Value(pair.second);
                });
            return true;
        }

        if (key == "resource_limits") {
            BuildYsonFluently(consumer)
                .Value(account->ClusterResourceLimits());
            return true;
        }

        if (key == "violated_resource_limits") {
            BuildYsonFluently(consumer)
                .BeginMap()
                    .Item("disk_space").Value(account->IsDiskSpaceLimitViolated())
                    .Item("node_count").Value(account->IsNodeCountLimitViolated())
                    .Item("chunk_count").Value(account->IsChunkCountLimitViolated())
                .EndMap();
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) override
    {
        auto* account = GetThisImpl();
        auto securityManager = Bootstrap_->GetSecurityManager();

        if (key == "resource_limits") {
            account->ClusterResourceLimits() = ConvertTo<TClusterResources>(value);
            return true;
        }

        if (key == "name") {
            auto newName = ConvertTo<Stroka>(value);
            securityManager->RenameAccount(account, newName);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
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

