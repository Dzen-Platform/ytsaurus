#include "cell_directory.h"
#include "config.h"
#include "private.h"

#include <yt/yt/ytlib/election/config.h>

#include <yt/yt/ytlib/hive/proto/cell_directory.pb.h>

#include <yt/yt/ytlib/hydra/config.h>
#include <yt/yt/ytlib/hydra/peer_channel.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <library/cpp/yt/threading/rw_spin_lock.h>

namespace NYT::NHiveClient {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;
using namespace NHydra;
using namespace NElection;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NRpc;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TCellPeerDescriptor::TCellPeerDescriptor()
    : Voting_(true)
{ }

TCellPeerDescriptor::TCellPeerDescriptor(const TNodeDescriptor& other, bool voting)
    : TNodeDescriptor(other)
    , Voting_(voting)
{ }

namespace {

TAddressMap ToAddressMap(const TCellPeerConfig& config, const TNetworkPreferenceList& networks)
{
    TAddressMap result;
    if (config.Address) {
        result.reserve(networks.size() + 1);
        for (const auto& network : networks) {
            EmplaceOrCrash(result, network, *config.Address);
        }
        // Default network must always be present in address map.
        result.emplace(DefaultNetworkName, *config.Address);
    }
    return result;
}

} // namespace

TCellPeerDescriptor::TCellPeerDescriptor(
    const TCellPeerConfig& config,
    const TNetworkPreferenceList& networks)
    : TNodeDescriptor(ToAddressMap(config, networks))
    , Voting_(config.Voting)
{ }

TCellPeerConfig TCellPeerDescriptor::ToConfig(const TNetworkPreferenceList& networks) const
{
    TCellPeerConfig config;
    config.Voting = Voting_;
    config.Address = IsNull() ? std::nullopt : std::make_optional(GetAddressOrThrow(networks));
    config.AlienCluster = AlienCluster_;
    return config;
}

////////////////////////////////////////////////////////////////////////////////

TCellDescriptor::TCellDescriptor(TCellId cellId)
    : CellId(cellId)
{ }

TCellConfigPtr TCellDescriptor::ToConfig(const TNetworkPreferenceList& networks) const
{
    auto config = New<TCellConfig>();
    config->CellId = CellId;
    config->Peers.reserve(Peers.size());
    for (const auto& peer : Peers) {
        config->Peers.emplace_back(peer.ToConfig(networks));
    }
    return config;
}

TCellInfo TCellDescriptor::ToInfo() const
{
    return TCellInfo{
        .CellId = CellId,
        .ConfigVersion = ConfigVersion
    };
}

void ToProto(NProto::TCellPeerDescriptor* protoDescriptor, const TCellPeerDescriptor& descriptor)
{
    ToProto(protoDescriptor->mutable_node_descriptor(), descriptor);
    protoDescriptor->set_voting(descriptor.GetVoting());
    if (descriptor.GetAlienCluster()) {
        protoDescriptor->set_alien_cluster(*descriptor.GetAlienCluster());
    } else {
        protoDescriptor->clear_alien_cluster();
    }
}

void FromProto(TCellPeerDescriptor* descriptor, const NProto::TCellPeerDescriptor& protoDescriptor)
{
    FromProto(descriptor, protoDescriptor.node_descriptor());
    descriptor->SetVoting(protoDescriptor.voting());
    descriptor->SetAlienCluster(protoDescriptor.has_alien_cluster()
        ? std::make_optional(protoDescriptor.alien_cluster())
        : std::nullopt);
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TCellInfo* protoInfo, const TCellInfo& info)
{
    ToProto(protoInfo->mutable_cell_id(), info.CellId);
    protoInfo->set_config_version(info.ConfigVersion);
}

void FromProto(TCellInfo* info, const NProto::TCellInfo& protoInfo)
{
    info->CellId = FromProto<TCellId>(protoInfo.cell_id());
    info->ConfigVersion = protoInfo.config_version();
}

void ToProto(NProto::TCellDescriptor* protoDescriptor, const TCellDescriptor& descriptor)
{
    ToProto(protoDescriptor->mutable_cell_id(), descriptor.CellId);
    protoDescriptor->set_config_version(descriptor.ConfigVersion);
    ToProto(protoDescriptor->mutable_peers(), descriptor.Peers);
}

void FromProto(TCellDescriptor* descriptor, const NProto::TCellDescriptor& protoDescriptor)
{
    descriptor->CellId = FromProto<TCellId>(protoDescriptor.cell_id());
    descriptor->ConfigVersion = protoDescriptor.config_version();
    descriptor->Peers = FromProto<std::vector<TCellPeerDescriptor>>(protoDescriptor.peers());
}

////////////////////////////////////////////////////////////////////////////////

class TCellDirectory
    : public ICellDirectory
{
public:
    TCellDirectory(
        TCellDirectoryConfigPtr config,
        IChannelFactoryPtr channelFactory,
        const TNetworkPreferenceList& networks,
        NLogging::TLogger logger)
        : Config_(std::move(config))
        , ChannelFactory_(std::move(channelFactory))
        , Networks_(networks)
        , Logger(std::move(logger))
    { }

    IChannelPtr FindChannelByCellId(TCellId cellId, EPeerKind peerKind) override
    {
        auto guard = ReaderGuard(SpinLock_);
        auto it = CellIdToEntry_.find(cellId);
        return it == CellIdToEntry_.end() ? nullptr : it->second.Channels[peerKind];
    }

    IChannelPtr GetChannelByCellIdOrThrow(TCellId cellId, EPeerKind peerKind) override
    {
        auto channel = FindChannelByCellId(cellId, peerKind);
        if (!channel) {
            THROW_ERROR_EXCEPTION("No cell with id %v is known",
                cellId);
        }
        return channel;
    }

    IChannelPtr GetChannelByCellId(TCellId cellId, EPeerKind peerKind) override
    {
        auto channel = FindChannelByCellId(cellId, peerKind);
        YT_VERIFY(channel);
        return channel;
    }

    IChannelPtr FindChannelByCellTag(TCellTag cellTag, EPeerKind peerKind) override
    {
        auto guard = ReaderGuard(SpinLock_);
        auto it = CellTagToEntry_.find(cellTag);
        return it == CellTagToEntry_.end() ? nullptr : it->second->Channels[peerKind];
    }

    IChannelPtr GetChannelByCellTagOrThrow(TCellTag cellTag, EPeerKind peerKind) override
    {
        auto channel = FindChannelByCellTag(cellTag, peerKind);
        if (!channel) {
            THROW_ERROR_EXCEPTION("No cell with tag %v is known",
                cellTag);
        }
        return channel;
    }

    IChannelPtr GetChannelByCellTag(TCellTag cellTag, EPeerKind peerKind) override
    {
        auto channel = FindChannelByCellTag(cellTag, peerKind);
        YT_VERIFY(channel);
        return channel;
    }

    std::vector<TCellInfo> GetRegisteredCells() override
    {
        auto guard = ReaderGuard(SpinLock_);
        std::vector<TCellInfo> result;
        result.reserve(CellIdToEntry_.size());
        for (const auto& [cellId, entry] : CellIdToEntry_) {
            result.push_back({cellId, entry.Descriptor.ConfigVersion});
        }
        return result;
    }

    bool IsCellUnregistered(TCellId cellId) override
    {
        auto guard = ReaderGuard(SpinLock_);
        return UnregisteredCellIds_.find(cellId) != UnregisteredCellIds_.end();
    }

    bool IsCellRegistered(TCellId cellId) override
    {
        auto guard = ReaderGuard(SpinLock_);
        return CellIdToEntry_.find(cellId) != CellIdToEntry_.end();
    }

    std::optional<TCellDescriptor> FindDescriptor(TCellId cellId) override
    {
        auto guard = ReaderGuard(SpinLock_);
        auto it = CellIdToEntry_.find(cellId);
        return it == CellIdToEntry_.end() ? std::nullopt : std::make_optional(it->second.Descriptor);
    }

    TCellDescriptor GetDescriptorOrThrow(TCellId cellId) override
    {
        auto result = FindDescriptor(cellId);
        if (!result) {
            THROW_ERROR_EXCEPTION("Unknown cell %v",
                cellId);
        }
        return *result;
    }

    std::optional<TString> FindPeerAddress(TCellId cellId, TPeerId peerId) override
    {
        auto guard = ReaderGuard(SpinLock_);
        auto it = CellIdToEntry_.find(cellId);
        if (!it) {
            return {};
        }

        const auto& peers = it->second.Descriptor.Peers;
        if (peerId >= std::ssize(peers)) {
            return {};
        }
        return peers[peerId].FindAddress(Networks_);
    }

    TSynchronizationResult Synchronize(const std::vector<TCellInfo>& knownCells) override
    {
        auto guard = ReaderGuard(SpinLock_);

        TSynchronizationResult result;

        int foundKnownCells = 0;
        for (const auto& knownCell : knownCells) {
            auto cellId = knownCell.CellId;
            if (auto it = CellIdToEntry_.find(cellId)) {
                const auto& entry = it->second;
                if (knownCell.ConfigVersion < entry.Descriptor.ConfigVersion) {
                    result.ReconfigureRequests.push_back({entry.Descriptor, knownCell.ConfigVersion});
                }
                ++foundKnownCells;
            } else {
                // NB: Currently we never request to unregister chaos cells; cf. YT-16393.
                if (TypeFromId(cellId) != EObjectType::ChaosCell) {
                    result.UnregisterRequests.push_back({
                        .CellId = cellId
                    });
                }
            }
        }

        // In most cases there are no missing cells in #knownCells.
        // Thus we may defer constructing #missingMap until we actually discover one.
        if (foundKnownCells < std::ssize(CellIdToEntry_)) {
            THashMap<TCellId, const TEntry*> missingMap;

            for (const auto& [cellId, entry] : CellIdToEntry_) {
                EmplaceOrCrash(missingMap, cellId, &entry);
            }

            for (const auto& knownCell : knownCells) {
                missingMap.erase(knownCell.CellId);
            }

            for (auto [cellId, entry] : missingMap) {
                result.ReconfigureRequests.push_back({
                    .NewDescriptor = entry->Descriptor,
                    .OldConfigVersion = -1
                });
            }
        }

        return result;
    }

    bool ReconfigureCell(TCellConfigPtr config, int configVersion) override
    {
        TCellDescriptor descriptor;
        descriptor.CellId = config->CellId;
        descriptor.ConfigVersion = configVersion;
        descriptor.Peers.reserve(config->Peers.size());
        for (const auto& peer : config->Peers) {
            descriptor.Peers.emplace_back(peer, Networks_);
        }
        return ReconfigureCell(descriptor);
    }

    bool ReconfigureCell(TPeerConnectionConfigPtr config, int configVersion) override
    {
        auto cellConfig = New<TCellConfig>();
        cellConfig->CellId = config->CellId;
        if (config->Addresses) {
            for (const auto& address : *config->Addresses) {
                cellConfig->Peers.emplace_back(address);
            }
        }
        return ReconfigureCell(cellConfig, configVersion);
    }

    bool ReconfigureCell(const TCellDescriptor& descriptor) override
    {
        auto guard = WriterGuard(SpinLock_);
        if (UnregisteredCellIds_.contains(descriptor.CellId)) {
            return false;
        }
        auto it = CellIdToEntry_.find(descriptor.CellId);
        if (it == CellIdToEntry_.end()) {
            it = CellIdToEntry_.emplace(descriptor.CellId, TEntry(descriptor)).first;
            auto* entry = &it->second;
            if (descriptor.ConfigVersion >= 0) {
                InitChannel(entry);
            }
            if (IsGlobalCellId(descriptor.CellId)) {
                auto cellTag = CellTagFromId(descriptor.CellId);
                if (auto [jt, inserted] = CellTagToEntry_.emplace(cellTag, entry); !inserted) {
                    YT_LOG_ALERT("Duplicate global cell id (CellTag: %v, ExistingCellId: %v, NewCellId: %v)",
                        cellTag,
                        jt->second->Descriptor.CellId,
                        descriptor.CellId);
                }
            }
            YT_LOG_DEBUG("Cell registered (CellId: %v, ConfigVersion: %v)",
                descriptor.CellId,
                descriptor.ConfigVersion);
            return true;
        } else if (it->second.Descriptor.ConfigVersion < descriptor.ConfigVersion) {
            it->second.Descriptor = descriptor;
            InitChannel(&it->second);
            YT_LOG_DEBUG("Cell reconfigured (CellId: %v, ConfigVersion: %v)",
                descriptor.CellId,
                descriptor.ConfigVersion);
            return true;
        }
        return false;
    }

    void RegisterCell(TCellId cellId) override
    {
        ReconfigureCell(TCellDescriptor(cellId));
    }

    bool UnregisterCell(TCellId cellId) override
    {
        auto guard = WriterGuard(SpinLock_);
        UnregisteredCellIds_.insert(cellId);
        if (CellIdToEntry_.erase(cellId) == 0) {
            return false;
        }
        if (IsGlobalCellId(cellId)) {
            EraseOrCrash(CellTagToEntry_, CellTagFromId(cellId));
        }
        YT_LOG_INFO("Cell unregistered (CellId: %v)",
            cellId);
        return true;
    }

    void Clear() override
    {
        auto guard = WriterGuard(SpinLock_);
        CellIdToEntry_.clear();
    }

private:
    const TCellDirectoryConfigPtr Config_;
    const IChannelFactoryPtr ChannelFactory_;
    const TNetworkPreferenceList Networks_;
    const NLogging::TLogger Logger;

    struct TEntry
    {
        explicit TEntry(const TCellDescriptor& descriptor)
            : Descriptor(descriptor)
        { }

        TCellDescriptor Descriptor;
        TEnumIndexedVector<EPeerKind, IChannelPtr> Channels;
    };

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, SpinLock_);
    THashMap<TCellId, TEntry> CellIdToEntry_;
    THashMap<TCellTag, TEntry*> CellTagToEntry_;
    THashSet<TCellId> UnregisteredCellIds_;


    void InitChannel(TEntry* entry)
    {
        auto peerConfig = New<TPeerConnectionConfig>();
        peerConfig->CellId = entry->Descriptor.CellId;
        peerConfig->Addresses.emplace();
        for (const auto& peer : entry->Descriptor.Peers) {
            if (!peer.IsNull()) {
                peerConfig->Addresses->push_back(peer.GetAddressOrThrow(Networks_));
            }
        }
        peerConfig->DiscoverTimeout = Config_->DiscoverTimeout;
        peerConfig->AcknowledgementTimeout = Config_->AcknowledgementTimeout;
        peerConfig->RediscoverPeriod = Config_->RediscoverPeriod;
        peerConfig->RediscoverSplay = Config_->RediscoverSplay;
        peerConfig->SoftBackoffTime = Config_->SoftBackoffTime;
        peerConfig->HardBackoffTime = Config_->HardBackoffTime;

        for (auto kind : TEnumTraits<EPeerKind>::GetDomainValues()) {
            entry->Channels[kind] = CreatePeerChannel(peerConfig, ChannelFactory_, kind);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

ICellDirectoryPtr CreateCellDirectory(
    TCellDirectoryConfigPtr config,
    IChannelFactoryPtr channelFactory,
    const NNodeTrackerClient::TNetworkPreferenceList& networks,
    NLogging::TLogger logger)
{
    return New<TCellDirectory>(
        std::move(config),
        std::move(channelFactory),
        networks,
        std::move(logger));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveClient
