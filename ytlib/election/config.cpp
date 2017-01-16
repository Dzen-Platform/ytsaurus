#include "config.h"

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NElection {

using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

TCellPeerConfig::TCellPeerConfig()
{ }

TCellPeerConfig::TCellPeerConfig(const TNullable<Stroka>& address, bool voting)
    : Address(address)
    , Voting(voting)
{ }

Stroka ToString(const TCellPeerConfig& config)
{
    TStringBuilder builder;
    builder.AppendFormat("%v", config.Address);
    if (!config.Voting) {
        builder.AppendString(" (non-voting)");
    }
    return builder.Flush();
}

void Serialize(const TCellPeerConfig& config, IYsonConsumer* consumer)
{
    if (!config.Voting) {
        consumer->OnBeginAttributes();
            consumer->OnKeyedItem("voting");
            consumer->OnBooleanScalar(false);
        consumer->OnEndAttributes();
    }
    if (config.Address) {
        consumer->OnStringScalar(*config.Address);
    } else {
        consumer->OnEntity();
    }
}

void Deserialize(TCellPeerConfig& config, INodePtr node)
{
    config.Address = node->GetType() == ENodeType::Entity ? Null : MakeNullable(node->GetValue<Stroka>());
    config.Voting = node->Attributes().Get<bool>("voting", true);
}

////////////////////////////////////////////////////////////////////////////////

TCellConfig::TCellConfig()
{
    RegisterParameter("cell_id", CellId);
    // TODO(babenko): rename to peers?
    RegisterParameter("addresses", Peers);

    RegisterValidator([&] () {
        auto type = TypeFromId(CellId);
        if (type != EObjectType::ClusterCell && type != EObjectType::TabletCell) {
            THROW_ERROR_EXCEPTION("Cell id %v has invalid type %Qlv",
                CellId,
                type);
        }

        auto cellTag = CellTagFromId(CellId);
        if (cellTag < MinValidCellTag || cellTag > MaxValidCellTag) {
            THROW_ERROR_EXCEPTION("Cell id %v has invalid cell tag",
                CellId);
        }

        int votingPeerCount = 0;
        for (const auto& peer : Peers) {
            if (peer.Voting) {
                ++votingPeerCount;
            }
        }

        if (votingPeerCount == 0) {
            THROW_ERROR_EXCEPTION("No voting peers found");
        }
    });
}

void TCellConfig::ValidateAllPeersPresent()
{
    for (int index = 0; index < Peers.size(); ++index) {
        if (!Peers[index].Address) {
            THROW_ERROR_EXCEPTION("Peer %v is missing in configuration of cell %v",
                index,
                CellId);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT

