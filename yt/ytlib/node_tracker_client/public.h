#pragma once

#include <yt/ytlib/object_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NNodeTrackerClient {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TNodeStatistics;
class TNodeResources;
class TNodeResourceLimitsOverrides;

class TNodeDescriptor;
class TNodeDirectory;

class TReqRegisterNode;
class TRspRegisterNode;

class TReqIncrementalHeartbeat;
class TRspIncrementalHeartbeat;

class TReqFullHeartbeat;
class TRspFullHeartbeat;

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

using TNodeId = i32;
const TNodeId InvalidNodeId = 0;
const TNodeId MaxNodeId = (1 << 24) - 1; // TNodeId must fit into 24 bits (see TChunkReplica)

using TRackId = NObjectClient::TObjectId;

using TDataCenterId = NObjectClient::TObjectId;

// Address type and value list.
using TAddressList = std::vector<std::pair<Stroka, Stroka>>;
using TNetworkPreferenceList = std::vector<Stroka>;
using TAddressMap = yhash_map<Stroka, Stroka>;

class TNodeDescriptor;

class TNodeDirectoryBuilder;

DECLARE_REFCOUNTED_CLASS(TNodeDirectory)

DECLARE_REFCOUNTED_STRUCT(INodeChannelFactory)

extern const Stroka DefaultNetworkName;
extern const TNetworkPreferenceList DefaultNetworkPreferences;

DEFINE_ENUM(EErrorCode,
    ((NoSuchNode)        (1600))
    ((InvalidState)      (1601))
    ((NoSuchNetwork)     (1602))
    ((NoSuchRack)        (1603))
    ((NoSuchDataCenter)  (1604))
);

DEFINE_ENUM(EMemoryCategory,
    ((Footprint)      (0))
    ((BlockCache)     (1))
    ((ChunkMeta)      (2))
    ((Jobs)           (3))
    ((TabletStatic)   (4))
    ((TabletDynamic)  (5))
    ((BlobSession)    (6))
);

///////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
