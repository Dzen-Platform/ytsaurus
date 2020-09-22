#include "helpers.h"

#include <yt/server/node/cluster_node/bootstrap.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/api/native/client.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/data_source.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/ytree/permission.h>

namespace NYT::NDataNode {

using namespace NApi;
using namespace NClusterNode;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NFileClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static constexpr int MaxChunksPerLocateRequest = 10000;

TFetchedArtifactKey FetchLayerArtifactKeyIfRevisionChanged(
    const NYPath::TYPath& path,
    NHydra::TRevision contentRevision,
    NClusterNode::TBootstrap const* bootstrap,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    TUserObject userObject;
    userObject.Path = path;

    {
        YT_LOG_INFO("Fetching layer basic attributes (LayerPath: %v, OldContentRevision: %llx)",
            path,
            contentRevision);

        TGetUserObjectBasicAttributesOptions options;
        options.SuppressAccessTracking = true;
        options.SuppressExpirationTimeoutRenewal = true;
        options.ReadFrom = EMasterChannelKind::Cache;
        GetUserObjectBasicAttributes(
            bootstrap->GetMasterClient(),
            {&userObject},
            NullTransactionId,
            Logger,
            EPermission::Read,
            options);

        if (userObject.Type != EObjectType::File) {
            THROW_ERROR_EXCEPTION("Invalid type of layer object %v: expected %Qlv, actual %Qlv",
                path,
                EObjectType::File,
                userObject.Type)
                << TErrorAttribute("path", path)
                << TErrorAttribute("expected_type", EObjectType::File)
                << TErrorAttribute("actual_type", userObject.Type);
        }
    }

    auto objectId = userObject.ObjectId;
    auto objectIdPath = FromObjectId(objectId);

    // TODO(max42): YT-13605.
    {
        YT_LOG_INFO("Fetching layer revision (LayerPath: %v, OldContentRevision: %llx)",
            path,
            contentRevision);

        TObjectServiceProxy proxy(bootstrap->GetMasterClient()->GetMasterChannelOrThrow(EMasterChannelKind::Cache));
        auto batchReq = proxy.ExecuteBatch();
        auto req = TYPathProxy::Get(objectIdPath + "/@content_revision");
        ToProto(req->mutable_attributes()->mutable_keys(), std::vector<TString>{"content_revision"});
        batchReq->AddRequest(req);

        try {
            auto resultYson = WaitFor(batchReq->Invoke())
                .ValueOrThrow()
                ->GetResponse<TYPathProxy::TRspGet>(0)
                .ValueOrThrow()
                ->value();

            userObject.ContentRevision = ConvertTo<NHydra::TRevision>(NYson::TYsonString(resultYson));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error fetching revision for layer %v", path)
                << ex;
        }
    }

    auto result = TFetchedArtifactKey {
        .ContentRevision = userObject.ContentRevision
    };

    if (contentRevision == userObject.ContentRevision) {
        YT_LOG_INFO("Layer revision not changed, using cached (LayerPath: %v, ObjectId: %v)",
            path,
            objectId);
        return result;
    }

    YT_LOG_INFO("Fetching layer chunk specs (LayerPath: %v, ObjectId: %v, ContentRevision: %llx)",
        path,
        objectId,
        userObject.ContentRevision);

    auto channel = bootstrap->GetMasterClient()->GetMasterChannelOrThrow(
        EMasterChannelKind::Cache,
        userObject.ExternalCellTag);
    TObjectServiceProxy proxy(channel);

    auto req = TFileYPathProxy::Fetch(objectIdPath);

    ToProto(req->mutable_ranges(), std::vector<TReadRange>{ {} });
    SetSuppressAccessTracking(req, true);
    SetSuppressExpirationTimeoutRenewal(req, true);
    req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);

    auto rspOrError = WaitFor(proxy.Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching chunks for layer %v", path);
    const auto& rsp = rspOrError.Value();

    std::vector<NChunkClient::NProto::TChunkSpec> chunkSpecs;
    ProcessFetchResponse(
        bootstrap->GetMasterClient(),
        rsp,
        userObject.ExternalCellTag,
        bootstrap->GetNodeDirectory(),
        MaxChunksPerLocateRequest,
        std::nullopt,
        Logger,
        &chunkSpecs);

    TArtifactKey layerKey;
    ToProto(layerKey.mutable_chunk_specs(), chunkSpecs);
    layerKey.mutable_data_source()->set_type(static_cast<int>(EDataSourceType::File));
    layerKey.mutable_data_source()->set_path(path);

    result.ArtifactKey = std::move(layerKey);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
