#include "helpers.h"
#include "private.h"
#include "config.h"
#include "chunk_slice.h"
#include "erasure_reader.h"
#include "replication_reader.h"

#include <yt/ytlib/api/client.h>

#include <yt/ytlib/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/cypress_client/rpc_helpers.cpp>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/compression/codec.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/address.h>

#include <array>

namespace NYT {
namespace NChunkClient {

using namespace NApi;
using namespace NRpc;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NErasure;
using namespace NNodeTrackerClient;
using namespace NProto;
using namespace NYTree;
using namespace NCypressClient;

using NYT::FromProto;
using NYT::ToProto;
using NNodeTrackerClient::TNodeId;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkClientLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkId CreateChunk(
    IClientPtr client,
    TCellTag cellTag,
    TMultiChunkWriterOptionsPtr options,
    const TTransactionId& transactionId,
    const TChunkListId& chunkListId,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    LOG_DEBUG("Creating chunk (ReplicationFactor: %v, TransactionId: %v, ChunkListId: %v)",
        options->ReplicationFactor,
        transactionId,
        chunkListId);

    auto chunkType = options->ErasureCodec == ECodec::None
        ? EObjectType::Chunk
        : EObjectType::ErasureChunk;

    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();
    GenerateMutationId(batchReq);

    auto* req = batchReq->add_create_chunk_subrequests();
    ToProto(req->mutable_transaction_id(), transactionId);
    req->set_type(static_cast<int>(chunkType));
    req->set_account(options->Account);
    req->set_replication_factor(options->ReplicationFactor);
    req->set_movable(options->ChunksMovable);
    req->set_vital(options->ChunksVital);
    req->set_erasure_codec(static_cast<int>(options->ErasureCodec));
    if (chunkListId) {
        ToProto(req->mutable_chunk_list_id(), chunkListId);
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        "Error creating chunk");

    const auto& batchRsp = batchRspOrError.Value();
    const auto& rsp = batchRsp->create_chunk_subresponses(0);
    return FromProto<TChunkId>(rsp.chunk_id());
}

void ProcessFetchResponse(
    IClientPtr client,
    TChunkOwnerYPathProxy::TRspFetchPtr fetchResponse,
    TCellTag fetchCellTag,
    TNodeDirectoryPtr nodeDirectory,
    int maxChunksPerLocateRequest,
    const NLogging::TLogger& logger,
    std::vector<NProto::TChunkSpec>* chunkSpecs)
{
    const auto& Logger = logger;

    nodeDirectory->MergeFrom(fetchResponse->node_directory());

    yhash_map<TCellTag, std::vector<NProto::TChunkSpec*>> foreignChunkMap;
    for (auto& chunkSpec : *fetchResponse->mutable_chunks()) {
        auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
        auto chunkCellTag = CellTagFromId(chunkId);
        if (chunkCellTag != fetchCellTag) {
            foreignChunkMap[chunkCellTag].push_back(&chunkSpec);
        }
    }

    for (const auto& pair : foreignChunkMap) {
        auto foreignCellTag = pair.first;
        auto& foreignChunkSpecs = pair.second;

        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower, foreignCellTag);
        TChunkServiceProxy proxy(channel);

        for (int beginIndex = 0; beginIndex < foreignChunkSpecs.size(); beginIndex += maxChunksPerLocateRequest) {
            int endIndex = std::min(
                beginIndex + maxChunksPerLocateRequest,
                static_cast<int>(foreignChunkSpecs.size()));

            auto req = proxy.LocateChunks();
            for (int index = beginIndex; index < endIndex; ++index) {
                *req->add_subrequests() = foreignChunkSpecs[index]->chunk_id();
            }

            LOG_DEBUG("Locating foreign chunks (CellTag: %v, ChunkCount: %v)",
                foreignCellTag,
                req->subrequests_size());

            auto rspOrError = WaitFor(req->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locating foreign chunks at cell %v",
                foreignCellTag);
            const auto& rsp = rspOrError.Value();
            YCHECK(req->subrequests_size() == rsp->subresponses_size());

            nodeDirectory->MergeFrom(rsp->node_directory());

            for (int globalIndex = beginIndex; globalIndex < endIndex; ++globalIndex) {
                int localIndex = globalIndex - beginIndex;
                const auto& subrequest = req->subrequests(localIndex);
                auto* subresponse = rsp->mutable_subresponses(localIndex);
                auto chunkId = FromProto<TChunkId>(subrequest);
                if (subresponse->missing()) {
                    THROW_ERROR_EXCEPTION(
                        NChunkClient::EErrorCode::NoSuchChunk,
                        "No such chunk %v",
                        chunkId);
                }
                foreignChunkSpecs[globalIndex]->mutable_replicas()->Swap(subresponse->mutable_replicas());
                foreignChunkSpecs[globalIndex]->set_erasure_codec(subresponse->erasure_codec());
            }
        }
    }

    for (auto& chunkSpec : *fetchResponse->mutable_chunks()) {
        chunkSpecs->push_back(NProto::TChunkSpec());
        chunkSpecs->back().Swap(&chunkSpec);
    }
}

TChunkReplicaList AllocateWriteTargets(
    NApi::IClientPtr client,
    const TChunkId& chunkId,
    int desiredTargetCount,
    int minTargetCount,
    TNullable<int> replicationFactorOverride,
    bool preferLocalHost,
    const std::vector<Stroka>& forbiddenAddresses,
    TNodeDirectoryPtr nodeDirectory,
    const NLogging::TLogger& logger)
{
    const auto& Logger = logger;

    LOG_DEBUG(
        "Allocating write targets "
        "(ChunkId: %v, DesiredTargetCount: %v, MinTargetCount: %v, PreferLocalHost: %v, "
        "ForbiddenAddresses: [%v])",
        chunkId,
        desiredTargetCount,
        minTargetCount,
        preferLocalHost,
        forbiddenAddresses);

    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader, CellTagFromId(chunkId));
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.AllocateWriteTargets();
    auto* req = batchReq->add_subrequests();
    req->set_desired_target_count(desiredTargetCount);
    req->set_min_target_count(minTargetCount);
    if (replicationFactorOverride) {
        req->set_replication_factor_override(*replicationFactorOverride);
    }
    if (preferLocalHost) {
        req->set_preferred_host_name(TAddressResolver::Get()->GetLocalHostName());
    }
    ToProto(req->mutable_forbidden_addresses(), forbiddenAddresses);
    ToProto(req->mutable_chunk_id(), chunkId);

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        batchRspOrError,
        NChunkClient::EErrorCode::MasterCommunicationFailed,
        "Error allocating targets for chunk %v",
        chunkId);
    const auto& batchRsp = batchRspOrError.Value();

    nodeDirectory->MergeFrom(batchRsp->node_directory());

    auto& rsp = batchRsp->subresponses(0);
    auto replicas = FromProto<TChunkReplicaList>(rsp.replicas());
    if (replicas.empty()) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::MasterCommunicationFailed,
            "Not enough data nodes available to write chunk %v",
            chunkId);
    }

    LOG_DEBUG("Write targets allocated (ChunkId: %v, Targets: %v)",
        chunkId,
        MakeFormattableRange(replicas, TChunkReplicaAddressFormatter(nodeDirectory)));

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

i64 GetChunkReaderMemoryEstimate(const TChunkSpec& chunkSpec, TMultiChunkReaderConfigPtr config)
{
    // Misc may be cleared out by the scheduler (e.g. for partition chunks).
    auto miscExt = FindProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());
    if (miscExt) {
        i64 currentSize;
        GetStatistics(chunkSpec, &currentSize);

        // Block used by upper level chunk reader.
        i64 chunkBufferSize = ChunkReaderMemorySize + miscExt->max_block_size();

        if (currentSize > miscExt->max_block_size()) {
            chunkBufferSize += config->WindowSize + config->GroupSize;
        }
        return chunkBufferSize;
    } else {
        return ChunkReaderMemorySize +
            config->WindowSize +
            config->GroupSize +
            DefaultMaxBlockSize;
    }
}

IChunkReaderPtr CreateRemoteReader(
    const TChunkSpec& chunkSpec,
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::IClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
{
    auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
    auto replicas = NYT::FromProto<TChunkReplicaList>(chunkSpec.replicas());

    if (IsErasureChunkId(chunkId)) {
        auto erasureCodecId = ECodec(chunkSpec.erasure_codec());
        LOG_DEBUG("Creating erasure remote reader (ChunkId: %v, Codec: %v)",
            chunkId,
            erasureCodecId);

        std::array<TNodeId, MaxTotalPartCount> partIndexToNodeId;
        std::fill(partIndexToNodeId.begin(), partIndexToNodeId.end(), InvalidNodeId);
        for (auto replica : replicas) {
            partIndexToNodeId[replica.GetIndex()] = replica.GetNodeId();
        }

        auto* erasureCodec = GetCodec(erasureCodecId);
        auto dataPartCount = erasureCodec->GetDataPartCount();

        std::vector<IChunkReaderPtr> readers;
        readers.reserve(dataPartCount);

        for (int index = 0; index < dataPartCount; ++index) {
            TChunkReplicaList partReplicas;
            auto nodeId = partIndexToNodeId[index];
            if (nodeId != InvalidNodeId) {
                partReplicas.push_back(TChunkReplica(nodeId, index));
            }

            auto partId = ErasurePartIdFromChunkId(chunkId, index);

            auto reader = CreateReplicationReader(
                config,
                options,
                client,
                nodeDirectory,
                localDescriptor,
                partId,
                partReplicas,
                blockCache,
                throttler);
            readers.push_back(reader);
        }

        return CreateNonRepairingErasureReader(readers);
    } else {
        LOG_DEBUG("Creating regular remote reader (ChunkId: %v)",
            chunkId);

        return CreateReplicationReader(
            config,
            options,
            client,
            nodeDirectory,
            localDescriptor,
            chunkId,
            replicas,
            blockCache,
            throttler);
    }
}

IChunkReaderPtr CreateRemoteReader(
    const TChunkId& chunkId,
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::IClientPtr client,
    const NNodeTrackerClient::TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler)
{
    auto channel = client->GetMasterChannelOrThrow(NApi::EMasterChannelKind::LeaderOrFollower);
    TChunkServiceProxy proxy(channel);

    auto req = proxy.LocateChunks();
    ToProto(req->add_subrequests(), chunkId);

    auto rsp = WaitFor(req->Invoke())
        .ValueOrThrow();

    const auto& subresponse = rsp->subresponses(0);
    TChunkSpec chunkSpec;
    ToProto(chunkSpec.mutable_chunk_id(), chunkId);
    chunkSpec.mutable_replicas()->MergeFrom(subresponse.replicas());
    chunkSpec.set_erasure_codec(subresponse.erasure_codec());

    auto nodeDirectory = New<TNodeDirectory>();
    nodeDirectory->MergeFrom(rsp->node_directory());

    return CreateRemoteReader(
        chunkSpec,
        config,
        options,
        client,
        nodeDirectory,
        localDescriptor,
        blockCache,
        throttler);
}

////////////////////////////////////////////////////////////////////////////////

void TUserObject::Persist(NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Path);
    Persist(context, ObjectId);
    Persist(context, CellTag);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
