#include "clique_authorization_manager.h"

#include "private.h"

#include <yt/ytlib/scheduler/helpers.h>

namespace NYT::NClickHouseServer {

using namespace NApi;
using namespace NYTree;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

class TCliqueAuthorizationManager
    : public ICliqueAuthorizationManager
{
public:
    TCliqueAuthorizationManager(
        NNative::IClientPtr client,
        TString cliqueId,
        bool validateOperationPermission)
        : Client_(std::move(client))
        , CliqueId_(std::move(cliqueId))
        , ValidateOperationPermission_(validateOperationPermission)
    { }

    virtual bool HasAccess(const std::string& user) override
    {
        if (!ValidateOperationPermission_) {
            return true;
        }

        try {
            NScheduler::ValidateOperationPermission(
                TString(user),
                TOperationId::FromString(CliqueId_),
                Client_,
                EPermission::Write,
                Logger);
            return true;
        } catch (const std::exception& ex) {
            YT_LOG_INFO(ex, "User does not have access to the containing operation (User: %v, OperationId: %v)",
                user,
                CliqueId_);
            return false;
        }
    }

private:
    IClientPtr Client_;
    TString CliqueId_;
    bool ValidateOperationPermission_ = false;
    const NLogging::TLogger& Logger = ServerLogger;
};

ICliqueAuthorizationManagerPtr CreateCliqueAuthorizationManager(
    NNative::IClientPtr client,
    TString cliqueId,
    bool validateOperationPermission)
{
    return std::make_shared<TCliqueAuthorizationManager>(std::move(client), std::move(cliqueId), validateOperationPermission);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
