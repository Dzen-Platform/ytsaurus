#include "admin_service.h"

#include "private.h"

#include <yt/ytlib/admin/admin_service_proxy.h>

#include <yt/core/misc/core_dumper.h>

#include <yt/core/rpc/service_detail.h>
#include <yt/core/rpc/public.h>

namespace NYT::NAdmin {

using namespace NLogging;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TAdminService
    : public TServiceBase
{
public:
    TAdminService(
        IInvokerPtr invoker,
        ICoreDumperPtr coreDumper)
        : TServiceBase(
            std::move(invoker),
            TAdminServiceProxy::GetDescriptor(),
            AdminLogger)
        , CoreDumper_(std::move(coreDumper))
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Die));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(WriteCoreDump));
    }

private:
    const ICoreDumperPtr CoreDumper_;

    void ValidateRoot(TStringBuf user)
    {
        if (user != RootUserName) {
            THROW_ERROR_EXCEPTION("Only root is allowed to use AdminService");
        }
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Die)
    {
        ValidateRoot(context->GetUser());

        _exit(request->exit_code());
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, WriteCoreDump)
    {
        ValidateRoot(context->GetUser());

        if (!CoreDumper_) {
            THROW_ERROR_EXCEPTION("Core dumper is not set up");
        }

        auto path = CoreDumper_->WriteCoreDump({
            "Reason: RPC",
            "RequestId: " + ToString(context->GetRequestId()),
        }, "rpc_call").Path;
        response->set_path(path);

        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

IServicePtr CreateAdminService(
    IInvokerPtr invoker,
    ICoreDumperPtr coreDumper)
{
    return New<TAdminService>(std::move(invoker), std::move(coreDumper));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAdmin
