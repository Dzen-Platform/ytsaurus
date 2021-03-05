#pragma once

#include "public.h"

#include <yt/yt/server/master/chunk_server/chunk_replica.h>

#include <yt/yt/client/node_tracker_client/proto/node.pb.h>

namespace NYT::NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

//! A helper for building node directories in fetch handlers.
class TNodeDirectoryBuilder
    : private TNonCopyable
{
public:
    explicit TNodeDirectoryBuilder(
        NNodeTrackerClient::NProto::TNodeDirectory* protoDirectory,
        NNodeTrackerClient::EAddressType addressType = NNodeTrackerClient::EAddressType::InternalRpc);

    void Add(const TNode* node);
    void Add(NChunkServer::TNodePtrWithIndexes node);
    void Add(const NChunkServer::TNodePtrWithIndexesList& nodes);

private:
    NNodeTrackerClient::NProto::TNodeDirectory* ProtoDirectory_;
    const NNodeTrackerClient::EAddressType AddressType_;

    THashSet<TNodeId> ListedNodeIds_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerServer
