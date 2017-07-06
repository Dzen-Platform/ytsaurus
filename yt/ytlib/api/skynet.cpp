#include "skynet.h"

#include "client.h"
#include "private.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/private.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/object_client/object_ypath_proxy.h>

#include <yt/ytlib/table_client/table_ypath_proxy.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ypath/public.h>

#include <yt/core/yson/consumer.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NCypressClient;

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TSkynetSharePartsLocations& skynetPartsLocations, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("nodes").Value(skynetPartsLocations.NodeDirectory)
            .Item("chunk_specs").DoListFor(
                skynetPartsLocations.ChunkSpecs,
                [&] (TFluentList fluent, const NChunkClient::NProto::TChunkSpec& spec) {
                    fluent
                        .Item()
                        .BeginMap()
                            .Item("chunk_id").Value(FromProto<TChunkId>(spec.chunk_id()))
                            .Item("row_index").Value(spec.table_row_index())
                            .Item("range_index").Value(spec.range_index())
                            .DoIf(spec.has_lower_limit(), [&] (TFluentMap fluent) {
                                fluent.Item("lower_limit").Value(FromProto<TReadLimit>(spec.lower_limit()));
                            })
                            .DoIf(spec.has_upper_limit(), [&] (TFluentMap fluent) {
                                fluent.Item("upper_limit").Value(FromProto<TReadLimit>(spec.upper_limit()));
                            })
                            .Item("replicas").DoListFor(spec.replicas(),
                                [] (TFluentList fluent, ui32 packedReplica) {
                                    TChunkReplica replica;
                                    FromProto(&replica, packedReplica);
                                    fluent.Item().Value(replica.GetNodeId());
                                })
                        .EndMap();
            })
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TSkynetSharePartsLocationsPtr DoLocateSkynetShare(
    INativeClientPtr client,
    const NYPath::TRichYPath& path,
    const TLocateSkynetShareOptions& options)
{
    auto& Logger = ApiLogger;

    TUserObject userObject;
    userObject.Path = path;

    GetUserObjectBasicAttributes(
        client,
        TMutableRange<TUserObject>(&userObject, 1),
        NullTransactionId,
        ChunkClientLogger,
        EPermission::Read,
        /* suppressAccessTracking */ false);

    const auto& objectId = userObject.ObjectId;
    const auto& tableCellTag = userObject.CellTag;

    auto objectIdPath = FromObjectId(objectId);

    if (userObject.Type != EObjectType::Table) {
        THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
            path,
            EObjectType::Table,
            userObject.Type);
    }

    int chunkCount;

    {
        LOG_INFO("Requesting chunk count");

        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);

        auto req = TYPathProxy::Get(objectIdPath + "/@");
        SetSuppressAccessTracking(req, false);
        std::vector<TString> attributeKeys{
            "chunk_count",
        };
        ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);

        auto rspOrError = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting table chunk count %v",
            path);

        const auto& rsp = rspOrError.Value();
        auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

        chunkCount = attributes->Get<int>("chunk_count");
    }

    auto skynetShareLocations = New<TSkynetSharePartsLocations>();

    LOG_INFO("Fetching table chunks");

    FetchChunkSpecs(
        client,
        skynetShareLocations->NodeDirectory,
        tableCellTag,
        path,
        objectId,
        path.GetRanges(),
        chunkCount,
        options.Config->MaxChunksPerFetch,
        options.Config->MaxChunksPerLocateRequest,
        [&] (TChunkOwnerYPathProxy::TReqFetchPtr req) {
            req->set_fetch_all_meta_extensions(false);
            SetSuppressAccessTracking(req, false);
        },
        ChunkClientLogger,
        &skynetShareLocations->ChunkSpecs);

    return skynetShareLocations;
}

TFuture<TSkynetSharePartsLocationsPtr> LocateSkynetShare(
    INativeClientPtr client,
    const NYPath::TRichYPath& path,
    const TLocateSkynetShareOptions& options)
{
    return BIND(DoLocateSkynetShare, client, path, options)
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
        .Run();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
