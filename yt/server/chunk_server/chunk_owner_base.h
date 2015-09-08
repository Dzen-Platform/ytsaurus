#pragma once

#include "public.h"
#include "private.h"

#include <ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <ytlib/chunk_client/data_statistics.pb.h>

#include <server/cypress_server/public.h>
#include <server/cypress_server/node.h>

#include <core/misc/property.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

// Base classes for cypress nodes that own chunks.
class TChunkOwnerBase
    : public NCypressServer::TCypressNodeBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(NChunkServer::TChunkList*, ChunkList);
    DEFINE_BYVAL_RW_PROPERTY(NChunkClient::EUpdateMode, UpdateMode);
    DEFINE_BYVAL_RW_PROPERTY(int, ReplicationFactor);
    DEFINE_BYVAL_RW_PROPERTY(bool, Vital);
    DEFINE_BYREF_RW_PROPERTY(NChunkClient::NProto::TDataStatistics, SnapshotStatistics);
    DEFINE_BYREF_RW_PROPERTY(NChunkClient::NProto::TDataStatistics, DeltaStatistics);

public:
    explicit TChunkOwnerBase(const NCypressServer::TVersionedNodeId& id);

    const TChunkList* GetSnapshotChunkList() const;
    const TChunkList* GetDeltaChunkList() const;

    virtual void BeginUpload(NChunkClient::EUpdateMode mode);
    virtual void EndUpload(
        const NChunkClient::NProto::TDataStatistics* statistics,
        const std::vector<Stroka>& keyColumns);
    virtual bool IsSorted() const;

    virtual NYTree::ENodeType GetNodeType() const override;

    NChunkClient::NProto::TDataStatistics ComputeTotalStatistics() const;

    virtual void Save(NCellMaster::TSaveContext& context) const override;
    virtual void Load(NCellMaster::TLoadContext& context) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
