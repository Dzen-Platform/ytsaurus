#include "chaos_replicated_table_node.h"

#include "chaos_cell_bundle.h"

#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/table_server/master_table_schema.h>

namespace NYT::NChaosServer {

using namespace NYTree;
using namespace NCellMaster;
using namespace NTableServer;
using namespace NSecurityServer;

////////////////////////////////////////////////////////////////////////////////

TChaosReplicatedTableNode* TChaosReplicatedTableNode::GetTrunkNode()
{
    return TCypressNode::GetTrunkNode()->As<TChaosReplicatedTableNode>();
}

const TChaosReplicatedTableNode* TChaosReplicatedTableNode::GetTrunkNode() const
{
    return TCypressNode::GetTrunkNode()->As<TChaosReplicatedTableNode>();
}

ENodeType TChaosReplicatedTableNode::GetNodeType() const
{
    return ENodeType::Entity;
}

TMasterTableSchema* TChaosReplicatedTableNode::GetSchema() const
{
    return Schema_;
}

void TChaosReplicatedTableNode::SetSchema(TMasterTableSchema* schema)
{
    Schema_ = schema;
}

TAccount* TChaosReplicatedTableNode::GetAccount() const
{
    return TCypressNode::GetAccount();
}

void TChaosReplicatedTableNode::Save(TSaveContext& context) const
{
    TCypressNode::Save(context);

    using NYT::Save;
    Save(context, ChaosCellBundle_);
    Save(context, ReplicationCardId_);
    Save(context, OwnsReplicationCard_);
    Save(context, Schema_);
}

void TChaosReplicatedTableNode::Load(TLoadContext& context)
{
    TCypressNode::Load(context);

    using NYT::Load;
    Load(context, ChaosCellBundle_);
    Load(context, ReplicationCardId_);
    Load(context, OwnsReplicationCard_);
    if (context.GetVersion() >= EMasterReign::ChaosReplicatedTableSchema) {
        Load(context, Schema_);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosServer
