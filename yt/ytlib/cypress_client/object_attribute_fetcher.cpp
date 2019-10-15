#include "object_attribute_fetcher.h"

#include <yt/ytlib/api/native/client.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/yson/string.h>

#include <yt/core/ytree/ypath_proxy.h>

namespace NYT::NObjectClient {

using namespace NYPath;
using namespace NApi;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TFuture<std::vector<TErrorOr<TAttributeMap>>> FetchAttributes(
    const std::vector<TYPath>& paths,
    const std::vector<TString>& attributes,
    const NNative::IClientPtr& client,
    const TMasterReadOptions& options)
{
    TObjectServiceProxy proxy(client->GetMasterChannelOrThrow(options.ReadFrom));
    auto batchReq = proxy.ExecuteBatch();
    for (const auto& path : paths) {
        auto req = TYPathProxy::Get(path + "/@");
        ToProto(req->mutable_attributes()->mutable_keys(), attributes);
        if (options.ReadFrom == EMasterChannelKind::Cache) {
            auto* cachingHeaderExt = req->Header().MutableExtension(NYTree::NProto::TCachingHeaderExt::caching_header_ext);
            cachingHeaderExt->set_success_expiration_time(ToProto<i64>(options.ExpireAfterSuccessfulUpdateTime));
            cachingHeaderExt->set_failure_expiration_time(ToProto<i64>(options.ExpireAfterFailedUpdateTime));

            auto* balancingHeaderExt = req->Header().MutableExtension(NRpc::NProto::TBalancingExt::balancing_ext);
            balancingHeaderExt->set_enable_stickness(true);
            balancingHeaderExt->set_sticky_group_size(options.CacheStickyGroupSize);
        }
        batchReq->AddRequest(req);
    }

    return batchReq->Invoke()
        .Apply(BIND([] (TObjectServiceProxy::TRspExecuteBatchPtr batchResponse) mutable {
            auto responses = batchResponse->GetResponses<TYPathProxy::TRspGet>();
            std::vector<TErrorOr<TAttributeMap>> result;
            result.reserve(responses.size());
            for (const auto& response : responses) {
                if (response.IsOK()) {
                    result.emplace_back(ConvertTo<TAttributeMap>(TYsonString(response.Value()->value())));
                } else {
                    result.emplace_back(TError(response));
                }
            }
            return result;
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // NYT::NObjectClient
