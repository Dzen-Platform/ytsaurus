#include "rpc_helpers.h"
#include "mutation_context.h"

#include <yt/core/misc/common.h>

#include <yt/core/rpc/service.h>

namespace NYT {
namespace NHydra {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

TCallback<void(const TErrorOr<TMutationResponse>&)> CreateRpcResponseHandler(IServiceContextPtr context)
{
    return BIND([=] (const TErrorOr<TMutationResponse>& result) {
        if (context->IsReplied())
            return;
        if (result.IsOK()) {
            const auto& response = result.Value();
            if (response.Data) {
                context->Reply(response.Data);
            } else {
                context->Reply(TError());
            }
        } else {
            context->Reply(result);
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
