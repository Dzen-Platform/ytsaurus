#pragma once

#include "public.h"
#include "chunk_owner_ypath_proxy.h"
#include "chunk_service_proxy.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/node_tracker_client/public.h>
#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/actions/public.h>

#include <yt/core/erasure/public.h>

#include <yt/core/rpc/public.h>

#include <yt/core/logging/public.h>

#include <yt/core/ytree/permission.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

NChunkClient::TChunkId CreateChunk(
    NApi::INativeClientPtr client,
    NObjectClient::TCellTag cellTag,
    TMultiChunkWriterOptionsPtr options,
    const NObjectClient::TTransactionId& transactionId,
    const TChunkListId& chunkListId,
    const NLogging::TLogger& logger);

//! Synchronously parses #fetchResponse, populates #nodeDirectory,
//! issues additional |LocateChunks| requests for foreign chunks.
//! The resulting chunk specs are appended to #chunkSpecs.
void ProcessFetchResponse(
    NApi::INativeClientPtr client,
    TChunkOwnerYPathProxy::TRspFetchPtr fetchResponse,
    NObjectClient::TCellTag fetchCellTag,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    int maxChunksPerLocateRequest,
    TNullable<int> rangeIndex,
    const NLogging::TLogger& logger,
    std::vector<NProto::TChunkSpec>* chunkSpecs);

//! Synchronously fetches chunk specs from master,
//! waits for thre result and processes the response.
//! The resulting chunk specs are appended to #chunkSpecs.
void FetchChunkSpecs(
    const NApi::INativeClientPtr& client,
    const NNodeTrackerClient::TNodeDirectoryPtr& nodeDirectory,
    NObjectClient::TCellTag cellTag,
    const NYPath::TRichYPath& path,
    const NObjectClient::TObjectId& objectId,
    const std::vector<TReadRange>& ranges,
    int chunkCount,
    int maxChunksPerFetch,
    int maxChunksPerLocateRequest,
    const std::function<void(TChunkOwnerYPathProxy::TReqFetchPtr)> initializeFetchRequest,
    const NLogging::TLogger& logger,
    std::vector<NProto::TChunkSpec>* chunkSpecs);

//! Synchronously invokes TChunkServiceProxy::AllocateWriteTargets.
//! Populates #nodeDirectory with the returned node descriptors.
//! Throws if the server returns no replicas.
TChunkReplicaList AllocateWriteTargets(
    NApi::INativeClientPtr client,
    const TChunkId& chunkId,
    int desiredTargetCount,
    int minTargetCount,
    TNullable<int> replicationFactorOverride,
    const Stroka& mediumName,
    bool preferLocalHost,
    const std::vector<Stroka>& forbiddenAddresses,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const NLogging::TLogger& logger);

//! Returns the cumulative error for the whole batch.
/*!
 *  If the envelope request fails then the corresponding error is returned.
 *  Otherwise, subresponses are examined and a cumulative error
 *  is constructed (with individual errors attached as inner).
 *  If all subresponses were successful then OK is returned.
 */
TError GetCumulativeError(const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError);

//! Locate all chunks passed in |chunkSpecList|.
/*! Chunks from |chunkSpecList| are updated to store information about active replicas.
 *  If nonnull |nodeDirectory| is provided it is also updated to store information
 *  about these replicas.
 */
void LocateChunks(
    NApi::INativeClientPtr client,
    int maxChunksPerLocateRequest,
    const std::vector<NProto::TChunkSpec*> chunkSpecList,
    const NNodeTrackerClient::TNodeDirectoryPtr& nodeDirectory,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

i64 GetChunkDataSize(const NProto::TChunkSpec& chunkSpec);
i64 GetChunkReaderMemoryEstimate(const NProto::TChunkSpec& chunkSpec, TMultiChunkReaderConfigPtr config);

IChunkReaderPtr CreateRemoteReader(
    const NProto::TChunkSpec& chunkSpec,
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::INativeClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const NNodeTrackerClient::TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler);

////////////////////////////////////////////////////////////////////////////////

struct TUserObject
{
    NYPath::TRichYPath Path;
    NObjectClient::TObjectId ObjectId;
    NObjectClient::TCellTag CellTag;
    NObjectClient::EObjectType Type = NObjectClient::EObjectType::Null;

    virtual ~TUserObject() = default;

    virtual Stroka GetPath() const;

    void Persist(const TStreamPersistenceContext& context);
};

template <class T>
void GetUserObjectBasicAttributes(
    NApi::INativeClientPtr client,
    TMutableRange<T> objects,
    const NObjectClient::TTransactionId& transactionId,
    const NLogging::TLogger& logger,
    NYTree::EPermission permission,
    bool suppressAccessTracking = false);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

#define HELPERS_INL_H_
#include "helpers-inl.h"
#undef HELPERS_INL_H_
