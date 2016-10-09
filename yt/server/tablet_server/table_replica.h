#pragma once

#include "public.h"

#include <yt/server/object_server/object.h>

#include <yt/server/table_server/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/enum.h>

#include <yt/core/ypath/public.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTableReplica
    : public NObjectServer::TObjectBase
    , public TRefTracked<TTableReplica>
{
public:
    DEFINE_BYVAL_RW_PROPERTY(Stroka, ClusterName);
    DEFINE_BYVAL_RW_PROPERTY(NYPath::TYPath, ReplicaPath);
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTimestamp, StartReplicationTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(NTableServer::TReplicatedTableNode*, Table);
    DEFINE_BYVAL_RW_PROPERTY(ETableReplicaState, State);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TTablet*>, DisablingTablets)

public:
    explicit TTableReplica(const TTableReplicaId& id);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void ThrowInvalidState();

    TDuration ComputeReplicationLagTime() const;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

