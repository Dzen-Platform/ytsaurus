#pragma once

#include "public.h"
#include "lock.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/cypress_server/cypress_manager.pb.h>

#include <yt/server/object_server/object.h>

#include <yt/server/security_server/acl.h>
#include <yt/server/security_server/cluster_resources.h>

#include <yt/server/transaction_server/public.h>

#include <yt/ytlib/cypress_client/public.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

#include <queue>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

//! Provides a common base for all versioned (aka Cypress) nodes.
class TCypressNodeBase
    : public NObjectServer::TObjectBase
    , public TRefTracked<TCypressNodeBase>
{
public:
    //! For external nodes, this is the tag of the cell were the node
    //! was delegated to. For non-external nodes, this is #NotReplicatedCellTag.
    DEFINE_BYVAL_RW_PROPERTY(NObjectClient::TCellTag, ExternalCellTag);

    //! If |false| then resource accounting ignores this node completely.
    //! Used by upload transactions, live preview etc.
    DEFINE_BYVAL_RW_PROPERTY(bool, AccountingEnabled);

    typedef yhash_map<NTransactionServer::TTransaction*, TTransactionLockState> TLockStateMap;
    DEFINE_BYREF_RW_PROPERTY(TLockStateMap, LockStateMap);

    typedef std::list<TLock*> TLockList;
    DEFINE_BYREF_RW_PROPERTY(TLockList, AcquiredLocks);
    DEFINE_BYREF_RW_PROPERTY(TLockList, PendingLocks);

    typedef yhash_set<TCypressNodeBase*> TNodeSet;
    //! Contains all nodes with parent pointing here.
    //! When a node dies parent pointers of its immediate descendants are reset.
    DEFINE_BYREF_RW_PROPERTY(TNodeSet, ImmediateDescendants);

    DEFINE_BYVAL_RW_PROPERTY(ELockMode, LockMode);

    DEFINE_BYVAL_RW_PROPERTY(TCypressNodeBase*, TrunkNode);

    DEFINE_BYVAL_RW_PROPERTY(NTransactionServer::TTransaction*, Transaction);

    DEFINE_BYVAL_RW_PROPERTY(TInstant, CreationTime);
    DEFINE_BYVAL_RW_PROPERTY(TInstant, ModificationTime);
    DEFINE_BYVAL_RW_PROPERTY(TInstant, AccessTime);

    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, ExpirationTime);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TCypressNodeExpirationMap::iterator>, ExpirationIterator);

    DEFINE_BYVAL_RW_PROPERTY(i64, AccessCounter);

    DEFINE_BYVAL_RW_PROPERTY(i64, Revision);

    DEFINE_BYVAL_RW_PROPERTY(NSecurityServer::TAccount*, Account);
    DEFINE_BYREF_RW_PROPERTY(NSecurityServer::TClusterResources, CachedResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(NSecurityServer::TAccessControlDescriptor, Acd);

    DEFINE_BYVAL_RW_PROPERTY(int, AccessStatisticsUpdateIndex);

    explicit TCypressNodeBase(const TVersionedNodeId& id);
    virtual ~TCypressNodeBase();

    //! Returns the static type of the node.
    /*!
     *  \see NYT::NYTree::INode::GetType
     */
    virtual NYTree::ENodeType GetNodeType() const = 0;

    TCypressNodeBase* GetParent() const;
    void SetParent(TCypressNodeBase* parent);
    void ResetParent();

    TCypressNodeBase* GetOriginator() const;
    void SetOriginator(TCypressNodeBase* originator);

    //! Returns the composite (versioned) id of the node.
    TVersionedNodeId GetVersionedId() const;

    //! Returns |true| if the node is external, i.e. was delegated
    //! to another cell.
    bool IsExternal() const;

    // Similar methods are also declared in TObjectBase but starting from TCypressNodeBase
    // they become virtual.
    virtual void Save(NCellMaster::TSaveContext& context) const;
    virtual void Load(NCellMaster::TLoadContext& context);

private:
    TCypressNodeBase* Parent_;
    TCypressNodeBase* Originator_;
    NTransactionServer::TTransactionId TransactionId_;

};

////////////////////////////////////////////////////////////////////////////////

struct TCypressNodeRefComparer
{
    static bool Compare(const TCypressNodeBase* lhs, const TCypressNodeBase* rhs);
};

NObjectServer::TVersionedObjectId GetObjectId(const TCypressNodeBase* object);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

#define NODE_INL_H_
#include "node-inl.h"
#undef NODE_INL_H_
