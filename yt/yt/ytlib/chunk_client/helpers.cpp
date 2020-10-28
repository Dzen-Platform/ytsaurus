#include "helpers.h"
#include "private.h"
#include "config.h"
#include "input_chunk_slice.h"
#include "replication_reader.h"
#include "repairing_reader.h"
#include "data_slice_descriptor.h"

#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/connection.h>
#include <yt/ytlib/api/native/config.h>
#include <yt/ytlib/api/native/rpc_helpers.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/cell_master_client/cell_directory.h>
#include <yt/ytlib/cell_master_client/cell_directory_synchronizer.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/client/chunk_client/chunk_replica.h>
#include <yt/client/chunk_client/data_statistics.h>

#include <yt/client/object_client/helpers.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/compression/codec.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/net/address.h>
#include <yt/core/net/local_address.h>

#include <yt/core/misc/statistics.h>

#include <array>

namespace NYT::NChunkClient {

using namespace NApi;
using namespace NRpc;
using namespace NConcurrency;
using namespace NLogging;
using namespace NObjectClient;
using namespace NErasure;
using namespace NNodeTrackerClient;
using namespace NYPath;
using namespace NYTree;
using namespace NNet;
using namespace NCypressClient;
using namespace NSecurityClient;

using NYT::FromProto;
using NYT::ToProto;
using NNodeTrackerClient::TNodeId;

////////////////////////////////////////////////////////////////////////////////

void ValidateReplicationFactor(int replicationFactor)
{
    if (replicationFactor < MinReplicationFactor || replicationFactor > MaxReplicationFactor)
    {
        THROW_ERROR_EXCEPTION("Replication factor %v is out of range [%v,%v]",
            replicationFactor,
            MinReplicationFactor,
            MaxReplicationFactor);
    }
}

TCellTag PickChunkHostingCell(
    const NApi::NNative::IConnectionPtr& connection,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    YT_LOG_DEBUG("Started synchronizing master cell directory");
    const auto& cellDirectorySynchronizer = connection->GetMasterCellDirectorySynchronizer();
    WaitFor(cellDirectorySynchronizer->RecentSync())
        .ThrowOnError();
    YT_LOG_DEBUG("Master cell directory synchronized successfully");

    const auto& cellDirectory = connection->GetMasterCellDirectory();
    auto cellId = cellDirectory->GetRandomMasterCellWithRoleOrThrow(NCellMasterClient::EMasterCellRoles::ChunkHost);
    return CellTagFromId(cellId);
}

void GetUserObjectBasicAttributes(
    const NApi::NNative::IClientPtr& client,
    const std::vector<TUserObject*>& objects,
    TTransactionId defaultTransactionId,
    const NLogging::TLogger& logger,
    EPermission permission,
    const TGetUserObjectBasicAttributesOptions& options)
{
    const auto& Logger = logger;

    YT_LOG_DEBUG("Getting basic attributes of user objects");

    auto channel = client->GetMasterChannelOrThrow(options.ReadFrom);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (auto* userObject : objects) {
        auto req = TObjectYPathProxy::GetBasicAttributes(userObject->GetObjectIdPathIfAvailable());
        req->set_permission(static_cast<int>(permission));
        req->set_omit_inaccessible_columns(options.OmitInaccessibleColumns);
        req->set_populate_security_tags(options.PopulateSecurityTags);
        if (auto optionalColumns = userObject->Path.GetColumns()) {
            auto* protoColumns = req->mutable_columns();
            for (const auto& column : *optionalColumns) {
                protoColumns->add_items(column);
            }
        }
        req->Tag() = userObject;
        NNative::SetCachingHeader(req, client->GetNativeConnection()->GetConfig(), options);
        NCypressClient::SetTransactionId(req, userObject->TransactionId.value_or(defaultTransactionId));
        NCypressClient::SetSuppressAccessTracking(req, options.SuppressAccessTracking);
        NCypressClient::SetSuppressExpirationTimeoutRenewal(req, options.SuppressExpirationTimeoutRenewal);
        batchReq->AddRequest(req);
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting basic attributes of user objects");
    const auto& batchRsp = batchRspOrError.Value();

    for (const auto& rspOrError : batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>()) {
        const auto& rsp = rspOrError.Value();
        auto* userObject = std::any_cast<TUserObject*>(rsp->Tag());
        userObject->ObjectId = FromProto<TObjectId>(rsp->object_id());
        userObject->ExternalCellTag = rsp->external_cell_tag();
        userObject->Type = TypeFromId(userObject->ObjectId);
        if (rsp->has_omitted_inaccessible_columns()) {
            userObject->OmittedInaccessibleColumns = FromProto<std::vector<TString>>(rsp->omitted_inaccessible_columns().items());
        }
        if (rsp->has_security_tags()) {
            userObject->SecurityTags = FromProto<std::vector<TSecurityTag>>(rsp->security_tags().items());
        }
        userObject->ExternalTransactionId = rsp->has_external_transaction_id()
            ? FromProto<TTransactionId>(rsp->external_transaction_id())
            : userObject->TransactionId.value_or(defaultTransactionId);
    }

    YT_LOG_DEBUG("Basic attributes received (Attributes: %v)",
        MakeFormattableView(objects, [] (auto* builder, const auto* object) {
            builder->AppendFormat("{Id: %v, ExternalCellTag: %v, ExternalTransactionId: %v}",
                object->ObjectId,
                object->ExternalCellTag,
                object->ExternalTransactionId);
        }));
}

TSessionId CreateChunk(
    const NNative::IClientPtr& client,
    TCellTag cellTag,
    const TMultiChunkWriterOptionsPtr& options,
    TTransactionId transactionId,
    TChunkListId chunkListId,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    YT_LOG_DEBUG("Creating chunk (ReplicationFactor: %v, TransactionId: %v, ChunkListId: %v, MediumName: %v)",
        options->ReplicationFactor,
        transactionId,
        chunkListId,
        options->MediumName);

    auto chunkType = options->ErasureCodec == ECodec::None
        ? EObjectType::Chunk
        : EObjectType::ErasureChunk;

    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();
    GenerateMutationId(batchReq);
    batchReq->set_suppress_upstream_sync(true);

    auto* req = batchReq->add_create_chunk_subrequests();
    ToProto(req->mutable_transaction_id(), transactionId);
    req->set_type(static_cast<int>(chunkType));
    req->set_account(options->Account);
    req->set_replication_factor(options->ReplicationFactor);
    req->set_movable(options->ChunksMovable);
    req->set_vital(options->ChunksVital);
    req->set_erasure_codec(static_cast<int>(options->ErasureCodec));
    req->set_medium_name(options->MediumName);
    req->set_validate_resource_usage_increase(options->ValidateResourceUsageIncrease);
    if (chunkListId) {
        ToProto(req->mutable_chunk_list_id(), chunkListId);
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        NChunkClient::EErrorCode::MasterCommunicationFailed,
        "Error creating chunk");

    const auto& batchRsp = batchRspOrError.Value();
    const auto& rsp = batchRsp->create_chunk_subresponses(0);
    auto sessionId = FromProto<TSessionId>(rsp.session_id());

    YT_LOG_DEBUG("Chunk created (MediumIndex: %v)",
        sessionId.MediumIndex);

    return sessionId;
}

void ProcessFetchResponse(
    const NNative::IClientPtr& client,
    const TChunkOwnerYPathProxy::TRspFetchPtr& fetchResponse,
    TCellTag fetchCellTag,
    const TNodeDirectoryPtr& nodeDirectory,
    int maxChunksPerLocateRequest,
    std::optional<int> rangeIndex,
    const NLogging::TLogger& logger,
    std::vector<NProto::TChunkSpec>* chunkSpecs,
    bool skipUnavailableChunks,
    EAddressType addressType)
{
    if (nodeDirectory) {
        nodeDirectory->MergeFrom(fetchResponse->node_directory());
    }

    std::vector<NProto::TChunkSpec*> foreignChunkSpecs;
    for (auto& chunkSpec : *fetchResponse->mutable_chunks()) {
        if (rangeIndex) {
            chunkSpec.set_range_index(*rangeIndex);
        }
        auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
        auto chunkCellTag = CellTagFromId(chunkId);
        if (chunkCellTag != fetchCellTag) {
            foreignChunkSpecs.push_back(&chunkSpec);
        }
    }

    LocateChunks(
        client,
        maxChunksPerLocateRequest,
        foreignChunkSpecs,
        nodeDirectory,
        logger,
        skipUnavailableChunks,
        addressType);

    for (auto& chunkSpec : *fetchResponse->mutable_chunks()) {
        chunkSpecs->push_back(std::move(chunkSpec));
    }
}

std::vector<NProto::TChunkSpec> FetchChunkSpecs(
    const NNative::IClientPtr& client,
    const TNodeDirectoryPtr& nodeDirectory,
    const TUserObject& userObject,
    const std::vector<NChunkClient::TReadRange>& ranges,
    int chunkCount,
    int maxChunksPerFetch,
    int maxChunksPerLocateRequest,
    const std::function<void(const TChunkOwnerYPathProxy::TReqFetchPtr&)>& initializeFetchRequest,
    const NLogging::TLogger& logger,
    bool skipUnavailableChunks,
    EAddressType addressType)
{
    std::vector<NProto::TChunkSpec> chunkSpecs;
    // XXX(babenko): YT-11825
    if (chunkCount >= 0) {
        chunkSpecs.reserve(static_cast<size_t>(chunkCount));
    }

    auto channel = client->GetMasterChannelOrThrow(
        EMasterChannelKind::Follower,
        userObject.ExternalCellTag);
    TObjectServiceProxy proxy(channel);
    auto batchReq = proxy.ExecuteBatch();

    for (int rangeIndex = 0; rangeIndex < static_cast<int>(ranges.size()); ++rangeIndex) {
        // XXX(babenko): YT-11825
        i64 subrequestCount = chunkCount < 0 ? 1 : (chunkCount + maxChunksPerFetch - 1) / maxChunksPerFetch;
        for (i64 subrequestIndex = 0; subrequestIndex < subrequestCount; ++subrequestIndex) {
            auto adjustedRange = ranges[rangeIndex];

            // XXX(babenko): YT-11825
            if (chunkCount >= 0) {
                auto chunkCountLowerLimit = subrequestIndex * maxChunksPerFetch;
                if (adjustedRange.LowerLimit().HasChunkIndex()) {
                    chunkCountLowerLimit = std::max(chunkCountLowerLimit, adjustedRange.LowerLimit().GetChunkIndex());
                }
                adjustedRange.LowerLimit().SetChunkIndex(chunkCountLowerLimit);

                auto chunkCountUpperLimit = (subrequestIndex + 1) * maxChunksPerFetch;
                if (adjustedRange.UpperLimit().HasChunkIndex()) {
                    chunkCountUpperLimit = std::min(chunkCountUpperLimit, adjustedRange.UpperLimit().GetChunkIndex());
                }
                adjustedRange.UpperLimit().SetChunkIndex(chunkCountUpperLimit);
            }

            // NB: objectId is null for virtual tables.
            auto req = TChunkOwnerYPathProxy::Fetch(userObject.GetObjectIdPathIfAvailable());
            AddCellTagToSyncWith(req, userObject.ObjectId);
            req->Tag() = rangeIndex;
            req->set_address_type(static_cast<int>(addressType));
            initializeFetchRequest(req.Get());
            ToProto(req->mutable_ranges(), std::vector<NChunkClient::TReadRange>{adjustedRange});
            batchReq->AddRequest(req);
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError),
        "Error fetching input table %v",
        userObject.GetPath());

    const auto& batchRsp = batchRspOrError.Value();
    auto rspsOrError = batchRsp->GetResponses<TChunkOwnerYPathProxy::TRspFetch>();

    for (const auto& rspOrError : rspsOrError) {
        const auto& rsp = rspOrError.Value();
        auto rangeIndex = std::any_cast<int>(rsp->Tag());
        ProcessFetchResponse(
            client,
            rspOrError.Value(),
            userObject.ExternalCellTag,
            nodeDirectory,
            maxChunksPerLocateRequest,
            rangeIndex,
            logger,
            &chunkSpecs,
            skipUnavailableChunks,
            addressType);
    }

    return chunkSpecs;
}

TChunkReplicaWithMediumList AllocateWriteTargets(
    const NNative::IClientPtr& client,
    TSessionId sessionId,
    int desiredTargetCount,
    int minTargetCount,
    std::optional<int> replicationFactorOverride,
    bool preferLocalHost,
    const std::vector<TString>& forbiddenAddresses,
    const TNodeDirectoryPtr& nodeDirectory,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    YT_LOG_DEBUG("Allocating write targets "
        "(ChunkId: %v, DesiredTargetCount: %v, MinTargetCount: %v, PreferLocalHost: %v, "
        "ForbiddenAddresses: %v)",
        sessionId,
        desiredTargetCount,
        minTargetCount,
        preferLocalHost,
        forbiddenAddresses);

    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader, CellTagFromId(sessionId.ChunkId));
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.AllocateWriteTargets();
    auto* req = batchReq->add_subrequests();
    req->set_desired_target_count(desiredTargetCount);
    req->set_min_target_count(minTargetCount);
    if (replicationFactorOverride) {
        req->set_replication_factor_override(*replicationFactorOverride);
    }
    if (preferLocalHost) {
        req->set_preferred_host_name(GetLocalHostName());
    }
    ToProto(req->mutable_forbidden_addresses(), forbiddenAddresses);
    ToProto(req->mutable_session_id(), sessionId);

    auto batchRspOrError = WaitFor(batchReq->Invoke());

    auto throwOnError = [&] (const TError& error) {
        THROW_ERROR_EXCEPTION_IF_FAILED(
            error,
            NChunkClient::EErrorCode::MasterCommunicationFailed,
            "Error allocating targets for chunk %v",
            sessionId);
    };

    throwOnError(batchRspOrError);
    const auto& batchRsp = batchRspOrError.Value();

    nodeDirectory->MergeFrom(batchRsp->node_directory());

    auto& rsp = batchRsp->subresponses(0);
    if (rsp.has_error()) {
        throwOnError(FromProto<TError>(rsp.error()));
    }

    auto replicas = FromProto<TChunkReplicaWithMediumList>(rsp.replicas());
    if (replicas.empty()) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::MasterCommunicationFailed,
            "Not enough data nodes available to write chunk %v",
            sessionId);
    }

    YT_LOG_DEBUG("Write targets allocated (ChunkId: %v, Targets: %v)",
        sessionId,
        MakeFormattableView(replicas, TChunkReplicaAddressFormatter(nodeDirectory)));

    return replicas;
}

TError GetCumulativeError(const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
{
    if (!batchRspOrError.IsOK()) {
        return batchRspOrError;
    }

    const auto& batchRsp = batchRspOrError.Value();
    TError cumulativeError("Error executing chunk operations");

    auto processSubresponses = [&] (const auto& subresponses) {
        for (const auto& subresponse : subresponses) {
            if (subresponse.has_error()) {
                cumulativeError.InnerErrors().push_back(FromProto<TError>(subresponse.error()));
            }
        }
    };
    processSubresponses(batchRsp->create_chunk_subresponses());
    processSubresponses(batchRsp->confirm_chunk_subresponses());
    processSubresponses(batchRsp->seal_chunk_subresponses());
    processSubresponses(batchRsp->create_chunk_lists_subresponses());
    processSubresponses(batchRsp->unstage_chunk_tree_subresponses());
    processSubresponses(batchRsp->attach_chunk_trees_subresponses());

    return cumulativeError.InnerErrors().empty() ? TError() : cumulativeError;
}

////////////////////////////////////////////////////////////////////////////////

i64 GetChunkDataWeight(const NProto::TChunkSpec& chunkSpec)
{
    if (chunkSpec.has_data_weight_override()) {
        return chunkSpec.data_weight_override();
    }
    const auto& miscExt = GetProtoExtension<NProto::TMiscExt>(chunkSpec.chunk_meta().extensions());
    return miscExt.data_weight();
}

i64 GetChunkUncompressedDataSize(const NProto::TChunkSpec& chunkSpec)
{
    const auto& miscExt = GetProtoExtension<NProto::TMiscExt>(chunkSpec.chunk_meta().extensions());
    return miscExt.uncompressed_data_size();
}

i64 GetChunkReaderMemoryEstimate(const NProto::TChunkSpec& chunkSpec, TMultiChunkReaderConfigPtr config)
{
    // Misc may be cleared out by the scheduler (e.g. for partition chunks).
    auto miscExt = FindProtoExtension<NProto::TMiscExt>(chunkSpec.chunk_meta().extensions());
    if (miscExt) {
        // NB: data weight is upper bound on the uncompressed data size.
        i64 currentSize = GetChunkDataWeight(chunkSpec);

        // Block used by upper level chunk reader.
        i64 chunkBufferSize = ChunkReaderMemorySize + miscExt->max_block_size();

        // If range to read is large enough to cover several blocks, consider prefetch memory estimate.
        if (currentSize > miscExt->max_block_size()) {
            chunkBufferSize += config->WindowSize + config->GroupSize;
        }

        // But after all we will not exceed total uncompressed data size for chunk.
        // Compressed data size is ignored (and works just fine according to psushin@).
        chunkBufferSize = std::min<i64>(chunkBufferSize, miscExt->uncompressed_data_size());

        return chunkBufferSize;
    } else {
        return ChunkReaderMemorySize +
            config->WindowSize +
            config->GroupSize +
            DefaultMaxBlockSize;
    }
}

IChunkReaderPtr CreateRemoteReader(
    const NProto::TChunkSpec& chunkSpec,
    TErasureReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NNative::IClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler)
{
    auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
    auto replicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());

    auto Logger = TLogger(ChunkClientLogger)
        .AddTag("ChunkId: %v", chunkId);

    if (IsErasureChunkId(chunkId)) {
        auto erasureCodecId = ECodec(chunkSpec.erasure_codec());
        YT_LOG_DEBUG("Creating erasure remote reader (Codec: %v)", erasureCodecId);

        std::array<TNodeId, ::NErasure::MaxTotalPartCount> partIndexToNodeId;
        std::fill(partIndexToNodeId.begin(), partIndexToNodeId.end(), InvalidNodeId);
        for (auto replica : replicas) {
            auto replicaIndex = replica.GetReplicaIndex();
            partIndexToNodeId[replicaIndex] = replica.GetNodeId();
        }

        auto* erasureCodec = GetCodec(erasureCodecId);
        auto partCount = config->EnableAutoRepair ?
            erasureCodec->GetTotalPartCount() :
            erasureCodec->GetDataPartCount();

        auto partConfig = CloneYsonSerializable(config);
        partConfig->FailOnNoSeeds = true;

        std::vector<IChunkReaderAllowingRepairPtr> readers;
        readers.reserve(partCount);

        for (int index = 0; index < partCount; ++index) {
            TChunkReplicaList partReplicas;
            auto nodeId = partIndexToNodeId[index];
            if (nodeId != InvalidNodeId) {
                partReplicas.push_back(TChunkReplica(nodeId, index));
            }

            auto partChunkId = ErasurePartIdFromChunkId(chunkId, index);
            auto reader = CreateReplicationReader(
                partConfig,
                options,
                client,
                nodeDirectory,
                localDescriptor,
                localNodeId,
                partChunkId,
                partReplicas,
                blockCache,
                trafficMeter,
                bandwidthThrottler,
                rpsThrottler);
            readers.push_back(reader);
        }

        return CreateRepairingReader(chunkId, erasureCodec, config, readers, Logger);
    } else {
        YT_LOG_DEBUG("Creating regular remote reader");

        return CreateReplicationReader(
            config,
            options,
            client,
            nodeDirectory,
            localDescriptor,
            localNodeId,
            chunkId,
            replicas,
            blockCache,
            trafficMeter,
            bandwidthThrottler,
            rpsThrottler);
    }
}

void LocateChunks(
    const NNative::IClientPtr& client,
    int maxChunksPerLocateRequest,
    const std::vector<NProto::TChunkSpec*>& chunkSpecList,
    const NNodeTrackerClient::TNodeDirectoryPtr& nodeDirectory,
    const NLogging::TLogger& logger,
    bool skipUnavailableChunks,
    EAddressType addressType)
{
    const auto& Logger = logger;

    THashMap<TCellTag, std::vector<NProto::TChunkSpec*>> chunkMap;

    for (auto* chunkSpec : chunkSpecList) {
        auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());
        auto chunkCellTag = CellTagFromId(chunkId);
        auto& cellChunkList = chunkMap[chunkCellTag];
        cellChunkList.push_back(chunkSpec);
    }

    for (auto& pair : chunkMap) {
        auto cellTag = pair.first;
        auto& chunkSpecs = pair.second;

        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Follower, cellTag);
        TChunkServiceProxy proxy(channel);

        for (int beginIndex = 0; beginIndex < chunkSpecs.size(); beginIndex += maxChunksPerLocateRequest) {
            int endIndex = std::min(
                beginIndex + maxChunksPerLocateRequest,
                static_cast<int>(chunkSpecs.size()));

            auto req = proxy.LocateChunks();
            req->SetHeavy(true);
            req->set_address_type(static_cast<int>(addressType));
            for (int index = beginIndex; index < endIndex; ++index) {
                *req->add_subrequests() = chunkSpecs[index]->chunk_id();
            }

            YT_LOG_DEBUG("Locating chunks (CellTag: %v, ChunkCount: %v)",
                cellTag,
                req->subrequests_size());

            auto rspOrError = WaitFor(req->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locating chunks at cell %v",
                cellTag);
            const auto& rsp = rspOrError.Value();
            YT_VERIFY(req->subrequests_size() == rsp->subresponses_size());

            if (nodeDirectory) {
                nodeDirectory->MergeFrom(rsp->node_directory());
            }

            for (int globalIndex = beginIndex; globalIndex < endIndex; ++globalIndex) {
                int localIndex = globalIndex - beginIndex;
                const auto& subrequest = req->subrequests(localIndex);
                auto* subresponse = rsp->mutable_subresponses(localIndex);
                auto chunkId = FromProto<TChunkId>(subrequest);
                if (subresponse->missing()) {
                    if (!skipUnavailableChunks) {
                        THROW_ERROR_EXCEPTION(
                            NChunkClient::EErrorCode::NoSuchChunk,
                            "No such chunk %v",
                            chunkId);
                    } else {
                        chunkSpecs[globalIndex]->mutable_replicas();
                    }
                } else {
                    chunkSpecs[globalIndex]->mutable_replicas()->Swap(subresponse->mutable_replicas());
                    chunkSpecs[globalIndex]->set_erasure_codec(subresponse->erasure_codec());
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TUserObject::TUserObject(
    TRichYPath path,
    std::optional<TTransactionId> transactionId)
    : Path(std::move(path))
    , TransactionId(transactionId)
{ }

bool TUserObject::IsPrepared() const
{
    return static_cast<bool>(ObjectId);
}

const NYPath::TYPath& TUserObject::GetPath() const
{
    return Path.GetPath();
}

TString TUserObject::GetObjectIdPath() const
{
    YT_VERIFY(IsPrepared());
    return FromObjectId(ObjectId);
}

TString TUserObject::GetObjectIdPathIfAvailable() const
{
    return ObjectId ? FromObjectId(ObjectId) : Path.GetPath();
}

void TUserObject::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Path);
    Persist(context, ObjectId);
    Persist(context, ExternalCellTag);
    Persist(context, ExternalTransactionId);
    Persist(context, Type);
    Persist(context, TransactionId);
    Persist(context, OmittedInaccessibleColumns);
    Persist(context, SecurityTags);
    // COMPAT(gritukan)
    if (context.GetVersion() >= 300302) {
        Persist(context, ChunkCount);
    }
}

////////////////////////////////////////////////////////////////////////////////

i64 CalculateDiskSpaceUsage(
    int replicationFactor,
    i64 regularDiskSpace,
    i64 erasureDiskSpace)
{
    // NB: replicationFactor == 0 for unused media.
    return replicationFactor > 0
        ? regularDiskSpace * replicationFactor + erasureDiskSpace
        : 0;
}

////////////////////////////////////////////////////////////////////////////////

void DumpCodecStatistics(
    const TCodecStatistics& codecStatistics,
    const NYPath::TYPath& path,
    TStatistics* statistics)
{
    for (const auto& pair : codecStatistics.CodecToDuration()) {
        statistics->AddSample(path + '/' + FormatEnum(pair.first), pair.second);
    }
}

////////////////////////////////////////////////////////////////////////////////

bool IsAddressLocal(const TString& address)
{
    return GetServiceHostName(address) == GetLocalHostName();
}

////////////////////////////////////////////////////////////////////////////////

TDataSliceSourcePair JoinDataSliceSourcePairs(std::vector<TDataSliceSourcePair> pairs)
{
    if (pairs.empty()) {
        return {};
    }

    TDataSliceSourcePair result = std::move(pairs.front());
    pairs.pop_back();

    size_t totalDataSliceCount = result.DataSliceDescriptors.size();
    size_t totalDataSourceCount = result.DataSourceDirectory->DataSources().size();
    for (const auto& pair : pairs) {
        totalDataSliceCount += pair.DataSliceDescriptors.size();
        totalDataSourceCount += pair.DataSourceDirectory->DataSources().size();
    }

    result.DataSliceDescriptors.reserve(totalDataSliceCount);
    result.DataSourceDirectory->DataSources().reserve(totalDataSourceCount);

    auto offset = result.DataSourceDirectory->DataSources().size();

    for (auto& pair : pairs) {
        for (auto& dataSlice : pair.DataSliceDescriptors) {
            for (auto& chunkSpec : dataSlice.ChunkSpecs) {
                chunkSpec.set_table_index(chunkSpec.table_index() + offset);
            }
            result.DataSliceDescriptors.emplace_back(std::move(dataSlice));
        }
        offset += pair.DataSourceDirectory->DataSources().size();
        for (auto& dataSource : pair.DataSourceDirectory->DataSources()) {
            result.DataSourceDirectory->DataSources().emplace_back(std::move(dataSource));
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
