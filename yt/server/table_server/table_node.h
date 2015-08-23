#pragma once

#include "public.h"

#include <core/misc/property.h>
#include <core/misc/small_vector.h>

#include <ytlib/chunk_client/chunk_owner_ypath_proxy.h>

#include <ytlib/table_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <server/chunk_server/chunk_owner_base.h>

#include <server/cypress_server/node_detail.h>

#include <server/tablet_server/public.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NTableServer {

////////////////////////////////////////////////////////////////////////////////

class TTableNode
    : public NChunkServer::TChunkOwnerBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(bool, Sorted);
    DEFINE_BYREF_RW_PROPERTY(NTableClient::TKeyColumns, KeyColumns);

    // For dynamic tables only.
    typedef std::vector<NTabletServer::TTablet*> TTabletList;
    typedef TTabletList::iterator TTabletListIterator; 
    DEFINE_BYREF_RW_PROPERTY(TTabletList, Tablets);

    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::EAtomicity, Atomicity);

public:
    explicit TTableNode(const NCypressServer::TVersionedNodeId& id);

    virtual NObjectClient::EObjectType GetObjectType() const;
    TTableNode* GetTrunkNode() const;

    virtual void BeginUpload(NChunkClient::EUpdateMode mode) override;
    virtual void EndUpload(
        const NChunkClient::NProto::TDataStatistics* statistics,
        const std::vector<Stroka>& keyColumns) override;
    virtual bool IsSorted() const override;

    virtual void Save(NCellMaster::TSaveContext& context) const override;
    virtual void Load(NCellMaster::TLoadContext& context) override;

    std::pair<TTabletListIterator, TTabletListIterator> GetIntersectingTablets(
        const NTableClient::TOwningKey& minKey,
        const NTableClient::TOwningKey& maxKey);

    bool IsDynamic() const;
    bool IsEmpty() const;
    bool HasMountedTablets() const;

};

////////////////////////////////////////////////////////////////////////////////

NCypressServer::INodeTypeHandlerPtr CreateTableTypeHandler(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

