#include "config.h"

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NElection {

using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

TCellPeerConfig::TCellPeerConfig()
{ }

TCellPeerConfig::TCellPeerConfig(const std::optional<TString>& address, bool voting)
    : Address(address)
    , Voting(voting)
{ }

TString ToString(const TCellPeerConfig& config)
{
    TStringBuilder builder;
    builder.AppendFormat("%v", config.Address);
    if (config.AlienCluster) {
        builder.AppendFormat("@%v", *config.AlienCluster);
    }
    if (!config.Voting) {
        builder.AppendString(" (non-voting)");
    }
    return builder.Flush();
}

void Serialize(const TCellPeerConfig& config, IYsonConsumer* consumer)
{
    if (!config.Voting || config.AlienCluster) {
        consumer->OnBeginAttributes();

        if (!config.Voting) {
            consumer->OnKeyedItem("voting");
            consumer->OnBooleanScalar(false);
        }

        if (config.AlienCluster) {
            consumer->OnKeyedItem("alien_cluster");
            consumer->OnStringScalar(*config.AlienCluster);
        }

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
    config.Address = node->GetType() == ENodeType::Entity ? std::nullopt : std::make_optional(node->GetValue<TString>());
    config.Voting = node->Attributes().Get<bool>("voting", true);
    config.AlienCluster = node->Attributes().Find<TString>("alien_cluster");
}

void Deserialize(TCellPeerConfig& config, TYsonPullParserCursor* cursor)
{
    config.Address.reset();
    config.Voting = true;
    config.AlienCluster.reset();

    if ((*cursor)->GetType() == EYsonItemType::BeginAttributes) {
        cursor->ParseAttributes([&](TYsonPullParserCursor* cursor) {
            auto key = (*cursor)->UncheckedAsString();
            if (key == "voting") {
                cursor->Next();
                config.Voting = ExtractTo<bool>(cursor);
            } else if (key == "alien_cluster") {
                cursor->Next();
                config.AlienCluster = ExtractTo<TString>(cursor);
            }
        });
    }
    if ((*cursor)->GetType() != EYsonItemType::EntityValue) {
        EnsureYsonToken("TCellPeerConfig", *cursor, EYsonItemType::StringValue);
        config.Address = ExtractTo<TString>(cursor);
    }
}

bool operator ==(const TCellPeerConfig& lhs, const TCellPeerConfig& rhs)
{
    return
        lhs.Address == rhs.Address &&
        lhs.Voting == rhs.Voting;
}

bool operator !=(const TCellPeerConfig& lhs, const TCellPeerConfig& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

TCellConfig::TCellConfig()
{
    RegisterParameter("cell_id", CellId);
    // TODO(babenko): rename to peers?
    RegisterParameter("addresses", Peers);

    RegisterPostprocessor([&] () {
        auto type = TypeFromId(CellId);
        if (type != EObjectType::MasterCell && type != EObjectType::TabletCell) {
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
    for (int index = 0; index < std::ssize(Peers); ++index) {
        if (!Peers[index].Address) {
            THROW_ERROR_EXCEPTION("Peer %v is missing in configuration of cell %v",
                index,
                CellId);
        }
    }
}

int TCellConfig::CountVotingPeers() const
{
    int votingPeerCount = 0;
    for (const auto& peer : Peers) {
        if (peer.Voting) {
            ++votingPeerCount;
        }
    }

    return votingPeerCount;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NElection

