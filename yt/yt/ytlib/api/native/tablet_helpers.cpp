#include "tablet_helpers.h"

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>

#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/helpers.h>

#include <yt/yt/core/rpc/hedging_channel.h>

namespace NYT::NApi::NNative {

using namespace NObjectClient;
using namespace NTabletClient;
using namespace NTableClient;
using namespace NHiveClient;
using namespace NRpc;
using namespace NHydra;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

TCompactVector<const TCellPeerDescriptor*, TypicalPeerCount> GetValidPeers(const TCellDescriptor& cellDescriptor)
{
    TCompactVector<const TCellPeerDescriptor*, TypicalPeerCount> peers;
    for (const auto& peer : cellDescriptor.Peers) {
        if (!peer.IsNull()) {
            peers.push_back(&peer);
        }
    }
    return peers;
}

const TCellPeerDescriptor& GetPrimaryTabletPeerDescriptor(
    const TCellDescriptor& cellDescriptor,
    EPeerKind peerKind)
{
    auto peers = GetValidPeers(cellDescriptor);

    if (peers.empty()) {
        THROW_ERROR_EXCEPTION("No alive replicas for tablet cell %v",
            cellDescriptor.CellId);
    }

    int leadingPeerIndex = -1;
    for (int index = 0; index < std::ssize(peers); ++index) {
        if (peers[index]->GetVoting()) {
            leadingPeerIndex = index;
            break;
        }
    }

    switch (peerKind) {
        case EPeerKind::Leader: {
            if (leadingPeerIndex < 0) {
                THROW_ERROR_EXCEPTION("No leading peer is known for tablet cell %v",
                    cellDescriptor.CellId);
            }
            return *peers[leadingPeerIndex];
        }

        case EPeerKind::LeaderOrFollower: {
            int randomIndex = RandomNumber(peers.size());
            return *peers[randomIndex];
        }

        case EPeerKind::Follower: {
            if (leadingPeerIndex < 0 || peers.size() == 1) {
                int randomIndex = RandomNumber(peers.size());
                return *peers[randomIndex];
            } else {
                int randomIndex = RandomNumber(peers.size() - 1);
                if (randomIndex >= leadingPeerIndex) {
                    ++randomIndex;
                }
                return *peers[randomIndex];
            }
        }

        default:
            YT_ABORT();
    }
}

const TCellPeerDescriptor& GetBackupTabletPeerDescriptor(
    const TCellDescriptor& cellDescriptor,
    const TCellPeerDescriptor& primaryPeerDescriptor)
{
    auto peers = GetValidPeers(cellDescriptor);

    YT_ASSERT(peers.size() > 1);

    int primaryPeerIndex = -1;
    for (int index = 0; index < std::ssize(peers); ++index) {
        if (peers[index] == &primaryPeerDescriptor) {
            primaryPeerIndex = index;
            break;
        }
    }

    YT_ASSERT(primaryPeerIndex >= 0 && primaryPeerIndex < std::ssize(peers));

    int randomIndex = RandomNumber(peers.size() - 1);
    if (randomIndex >= primaryPeerIndex) {
        ++randomIndex;
    }

    return *peers[randomIndex];
}

IChannelPtr CreateTabletReadChannel(
    const IChannelFactoryPtr& channelFactory,
    const TCellDescriptor& cellDescriptor,
    const TTabletReadOptions& options,
    const TNetworkPreferenceList& networks)
{
    const auto& primaryPeerDescriptor = GetPrimaryTabletPeerDescriptor(cellDescriptor, options.ReadFrom);
    auto primaryChannel = channelFactory->CreateChannel(primaryPeerDescriptor.GetAddressWithNetworkOrThrow(networks));
    if (cellDescriptor.Peers.size() == 1 || !options.RpcHedgingDelay) {
        return primaryChannel;
    }

    const auto& backupPeerDescriptor = GetBackupTabletPeerDescriptor(cellDescriptor, primaryPeerDescriptor);
    auto backupChannel = channelFactory->CreateChannel(backupPeerDescriptor.GetAddressWithNetworkOrThrow(networks));

    return CreateHedgingChannel(
        std::move(primaryChannel),
        std::move(backupChannel),
        THedgingChannelOptions{
            .Delay = *options.RpcHedgingDelay
        });
}

void ValidateTabletMountedOrFrozen(const TTabletInfoPtr& tabletInfo)
{
    auto state = tabletInfo->State;
    if (state != ETabletState::Mounted &&
        state != ETabletState::Freezing &&
        state != ETabletState::Unfreezing &&
        state != ETabletState::Frozen)
    {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::TabletNotMounted,
            "Cannot read from tablet %v while it is in %Qlv state",
            tabletInfo->TabletId,
            state)
            << TErrorAttribute("tablet_id", tabletInfo->TabletId)
            << TErrorAttribute("is_tablet_unmounted", state == ETabletState::Unmounted);
    }
}

void ValidateTabletMounted(const TTableMountInfoPtr& tableInfo, const TTabletInfoPtr& tabletInfo)
{
    auto state = tabletInfo->State;
    if (state != ETabletState::Mounted) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::TabletNotMounted,
            "Tablet %v of table %v is in %Qlv state",
            tabletInfo->TabletId,
            tableInfo->Path,
            tabletInfo->State)
            << TErrorAttribute("tablet_id", tabletInfo->TabletId)
            << TErrorAttribute("is_tablet_unmounted", state == ETabletState::Unmounted);
    }
}

void ValidateTabletMounted(
    const TTableMountInfoPtr& tableInfo,
    const TTabletInfoPtr& tabletInfo,
    bool validateWrite)
{
    if (validateWrite) {
        ValidateTabletMounted(tableInfo, tabletInfo);
    } else {
        ValidateTabletMountedOrFrozen(tabletInfo);
    }
}

TNameTableToSchemaIdMapping BuildColumnIdMapping(
    const TTableSchema& schema,
    const TNameTablePtr& nameTable)
{
    for (const auto& name : schema.GetKeyColumns()) {
        // We shouldn't consider computed columns below because client doesn't send them.
        if (!nameTable->FindId(name) && !schema.GetColumnOrThrow(name).Expression()) {
            THROW_ERROR_EXCEPTION("Missing key column %Qv",
                name);
        }
    }

    TNameTableToSchemaIdMapping mapping;
    mapping.resize(nameTable->GetSize());
    for (int nameTableId = 0; nameTableId < nameTable->GetSize(); ++nameTableId) {
        const auto& name = nameTable->GetName(nameTableId);
        const auto* columnSchema = schema.FindColumn(name);
        mapping[nameTableId] = columnSchema ? schema.GetColumnIndex(*columnSchema) : -1;
    }
    return mapping;
}

namespace {

template <class TRow>
TTabletInfoPtr GetSortedTabletForRowImpl(
    const TTableMountInfoPtr& tableInfo,
    TRow row,
    bool validateWrite)
{
    YT_ASSERT(tableInfo->IsSorted());

    auto tabletInfo = tableInfo->GetTabletForRow(row);
    ValidateTabletMounted(tableInfo, tabletInfo, validateWrite);
    return tabletInfo;
}

} // namespace

TTabletInfoPtr GetSortedTabletForRow(
    const TTableMountInfoPtr& tableInfo,
    TUnversionedRow row,
    bool validateWrite)
{
    return GetSortedTabletForRowImpl(tableInfo, row, validateWrite);
}

TTabletInfoPtr GetSortedTabletForRow(
    const TTableMountInfoPtr& tableInfo,
    TVersionedRow row,
    bool validateWrite)
{
    return GetSortedTabletForRowImpl(tableInfo, row, validateWrite);
}

TTabletInfoPtr GetOrderedTabletForRow(
    const TTableMountInfoPtr& tableInfo,
    const TTabletInfoPtr& randomTabletInfo,
    std::optional<int> tabletIndexColumnId,
    TUnversionedRow row,
    bool validateWrite)
{
    YT_ASSERT(!tableInfo->IsSorted());

    i64 tabletIndex = -1;
    for (const auto& value : row) {
        if (tabletIndexColumnId && value.Id == *tabletIndexColumnId && value.Type != EValueType::Null) {
            try {
                FromUnversionedValue(&tabletIndex, value);
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Error parsing tablet index from row")
                    << ex;
            }
            if (tabletIndex < 0 || tabletIndex >= std::ssize(tableInfo->Tablets)) {
                THROW_ERROR_EXCEPTION("Invalid tablet index: actual %v, expected in range [0, %v]",
                    tabletIndex,
                    tableInfo->Tablets.size() - 1);
            }
        }
    }

    if (tabletIndex < 0) {
        return randomTabletInfo;
    }

    auto tabletInfo = tableInfo->Tablets[tabletIndex];
    ValidateTabletMounted(tableInfo, tabletInfo, validateWrite);
    return tabletInfo;
}


////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
