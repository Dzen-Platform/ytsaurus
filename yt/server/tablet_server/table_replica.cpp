#include "table_replica.h"
#include "tablet.h"

#include <yt/server/table_server/replicated_table_node.h>

#include <yt/server/cell_master/serialize.h>

namespace NYT {
namespace NTabletServer {

using namespace NYPath;
using namespace NTableServer;

////////////////////////////////////////////////////////////////////////////////

TTableReplica::TTableReplica(const TTableReplicaId& id)
    : TObjectBase(id)
{ }

void TTableReplica::Save(NCellMaster::TSaveContext& context) const
{
    TObjectBase::Save(context);

    using NYT::Save;
    Save(context, ClusterName_);
    Save(context, ReplicaPath_);
    Save(context, StartReplicationTimestamp_);
    Save(context, Table_);
    Save(context, DisablingTablets_);
}

void TTableReplica::Load(NCellMaster::TLoadContext& context)
{
    TObjectBase::Load(context);

    using NYT::Load;
    Load(context, ClusterName_);
    Load(context, ReplicaPath_);
    Load(context, StartReplicationTimestamp_);
    Load(context, Table_);
    Load(context, DisablingTablets_);
}

void TTableReplica::ThrowInvalidState()
{
    THROW_ERROR_EXCEPTION("Table replica %v is in %Qlv state",
        Id_,
        State_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

