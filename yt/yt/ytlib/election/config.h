#pragma once

#include "public.h"

#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NElection {

////////////////////////////////////////////////////////////////////////////////

struct TCellPeerConfig
{
    TCellPeerConfig();
    explicit TCellPeerConfig(const std::optional<TString>& address, bool voting = true);

    std::optional<TString> Address;
    std::optional<TString> AlienCluster;
    bool Voting = true;
};

TString ToString(const TCellPeerConfig& config);

void Serialize(const TCellPeerConfig& config, NYson::IYsonConsumer* consumer);
void Deserialize(TCellPeerConfig& config, NYTree::INodePtr node);
void Deserialize(TCellPeerConfig& config, NYson::TYsonPullParserCursor* cursor);

bool operator ==(const TCellPeerConfig& lhs, const TCellPeerConfig& rhs);
bool operator !=(const TCellPeerConfig& lhs, const TCellPeerConfig& rhs);

////////////////////////////////////////////////////////////////////////////////

class TCellConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Cell id; an arbitrary random object id of |Cell| type.
    TCellId CellId;

    //! Peer addresses.
    //! Some could be Null to indicate that the peer is temporarily missing.
    std::vector<TCellPeerConfig> Peers;

    TCellConfig();

    void ValidateAllPeersPresent();

    int CountVotingPeers() const;
};

DEFINE_REFCOUNTED_TYPE(TCellConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NElection
