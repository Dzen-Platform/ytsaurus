#include "replicated_table_node.h"
#include "table_node_type_handler_detail.h"

#include <yt/server/tablet_server/table_replica.h>

namespace NYT {
namespace NTableServer {

using namespace NObjectClient;
using namespace NCypressServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

TReplicatedTableNode::TReplicatedTableNode(const TVersionedNodeId& id)
    : TTableNode(id)
{ }

EObjectType TReplicatedTableNode::GetObjectType() const
{
    return EObjectType::ReplicatedTable;
}

void TReplicatedTableNode::Save(TSaveContext& context) const
{
    TTableNode::Save(context);

    using NYT::Save;
    Save(context, Replicas_);
}

void TReplicatedTableNode::Load(TLoadContext& context)
{
    TTableNode::Load(context);

    using NYT::Load;
    Load(context, Replicas_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

