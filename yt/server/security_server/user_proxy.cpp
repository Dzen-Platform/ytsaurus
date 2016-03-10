#include "user_proxy.h"
#include "security_manager.h"
#include "subject_proxy_detail.h"
#include "user.h"

#include <yt/ytlib/security_client/user_ypath.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TUserProxy
    : public TSubjectProxy<TUser>
{
public:
    TUserProxy(NCellMaster::TBootstrap* bootstrap, TUser* user)
        : TBase(bootstrap, user)
    { }

private:
    typedef TSubjectProxy<TUser> TBase;

    virtual void ValidateRemoval() override
    {
        const auto* user = GetThisTypedImpl();
        if (user->IsBuiltin())  {
            THROW_ERROR_EXCEPTION("Cannot remove a built-in user %Qv",
                user->GetName());
        }
    }

    virtual void ListSystemAttributes(std::vector<ISystemAttributeProvider::TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back(TAttributeDescriptor("banned")
            .SetReplicated(true));
        descriptors->push_back(TAttributeDescriptor("request_rate_limit")
            .SetReplicated(true));
        descriptors->push_back("access_time");
        descriptors->push_back("request_count");
        descriptors->push_back("read_request_time");
        descriptors->push_back("write_request_time");
        descriptors->push_back(TAttributeDescriptor("multicell_statistics")
            .SetOpaque(true));
        descriptors->push_back("request_rate");
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        auto* user = GetThisTypedImpl();
        auto securityManager = Bootstrap_->GetSecurityManager();

        if (key == "banned") {
            BuildYsonFluently(consumer)
                .Value(user->GetBanned());
            return true;
        }

        if (key == "request_rate_limit") {
            BuildYsonFluently(consumer)
                .Value(user->GetRequestRateLimit());
            return true;
        }

        if (key == "access_time") {
            BuildYsonFluently(consumer)
                .Value(user->ClusterStatistics().AccessTime);
            return true;
        }

        if (key == "request_count") {
            BuildYsonFluently(consumer)
                .Value(user->ClusterStatistics().RequestCount);
            return true;
        }

        if (key == "read_request_time") {
            BuildYsonFluently(consumer)
                .Value(user->ClusterStatistics().ReadRequestTime);
            return true;
        }

        if (key == "write_request_time") {
            BuildYsonFluently(consumer)
                .Value(user->ClusterStatistics().WriteRequestTime);
            return true;
        }

        if (key == "multicell_statistics") {
            BuildYsonFluently(consumer)
                .DoMapFor(user->MulticellStatistics(), [] (TFluentMap fluent, const std::pair<TCellTag, const TUserStatistics&>& pair) {
                    fluent.Item(ToString(pair.first)).Value(pair.second);
                });
            return true;
        }

        if (key == "request_rate") {
            BuildYsonFluently(consumer)
                .Value(securityManager->GetRequestRate(user));
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const TYsonString& value) override
    {
        auto* user = GetThisTypedImpl();
        auto securityManager = Bootstrap_->GetSecurityManager();

        if (key == "banned") {
            auto banned = ConvertTo<bool>(value);
            securityManager->SetUserBanned(user, banned);
            return true;
        }

        if (key == "request_rate_limit") {
            auto limit = ConvertTo<double>(value);
            if (limit < 0) {
                THROW_ERROR_EXCEPTION("\"request_rate_limit\" cannot be negative");
            }
            user->SetRequestRateLimit(limit);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }

};

IObjectProxyPtr CreateUserProxy(
    NCellMaster::TBootstrap* bootstrap,
    TUser* user)
{
    return New<TUserProxy>(bootstrap, user);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT

