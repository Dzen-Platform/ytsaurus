#include "orchid_service.h"
#include "private.h"
#include "orchid_service_proxy.h"

#include <yt/core/misc/common.h>

#include <yt/core/rpc/message.h>

#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_detail.h>

namespace NYT {
namespace NOrchid {

using namespace NBus;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TOrchidService
    : public TServiceBase
{
public:
    TOrchidService(
        INodePtr root,
        IInvokerPtr invoker)
        : TServiceBase(
            invoker,
            TOrchidServiceProxy::GetServiceName(),
            OrchidLogger)
    {
        YCHECK(root);

        RootService_ = CreateRootService(root);
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute));
    }

private:
    IYPathServicePtr RootService_;

    DECLARE_RPC_SERVICE_METHOD(NProto, Execute)
    {
        auto requestMessage = TSharedRefArray(request->Attachments());

        TRequestHeader requestHeader;
        if (!ParseRequestHeader(requestMessage, &requestHeader)) {
            THROW_ERROR_EXCEPTION("Error parsing request header");
        }

        context->SetRequestInfo("%v:%v %v",
            requestHeader.service(),
            requestHeader.method(),
            GetRequestYPath(context));

        ExecuteVerb(RootService_, requestMessage)
            .Subscribe(BIND([=] (const TErrorOr<TSharedRefArray>& responseMessageOrError) {
                if (!responseMessageOrError.IsOK()) {
                    context->Reply(responseMessageOrError);
                    return;
                }

                const auto& responseMessage = responseMessageOrError.Value();
                TResponseHeader responseHeader;
                YCHECK(ParseResponseHeader(responseMessage, &responseHeader));

                auto error = FromProto<TError>(responseHeader.error());

                context->SetResponseInfo("InnerError: %v", error);

                response->Attachments() = responseMessage.ToVector();
                context->Reply();
            }));
    }

};

IServicePtr CreateOrchidService(
    INodePtr root,
    IInvokerPtr invoker)
{
    return New<TOrchidService>(
        root,
        invoker);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT

