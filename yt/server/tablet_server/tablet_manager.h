#pragma once

#include "public.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/chunk_server/chunk_tree_statistics.h>

#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/public.h>

#include <yt/server/table_server/public.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/table_client/public.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletManager
    : public TRefCounted
{
public:
    explicit TTabletManager(
        TTabletManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TTabletManager();

    void Initialize();

    int GetAssignedTabletCellCount(const Stroka& address) const;

    NTableClient::TTableSchema GetTableSchema(NTableServer::TTableNode* table);

    TTabletStatistics GetTabletStatistics(const TTablet* tablet);


    void MountTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const TTabletCellId& cellId,
        i64 estimatedUncompressedSize,
        i64 estimatedCompressedSize);

    void UnmountTable(
        NTableServer::TTableNode* table,
        bool force,
        int firstTabletIndex = -1,
        int lastTabletIndex = -1);

    void RemountTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex = -1,
        int lastTabletIndex = -1);

    void ClearTablets(
        NTableServer::TTableNode* table);

    void ReshardTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const std::vector<NTableClient::TOwningKey>& pivotKeys);


    DECLARE_ENTITY_MAP_ACCESSORS(TabletCellBundle, TTabletCellBundle, TTabletCellBundleId);
    TTabletCellBundle* FindTabletCellBundleByName(const Stroka& name);

    DECLARE_ENTITY_MAP_ACCESSORS(TabletCell, TTabletCell, TTabletCellId);
    TTabletCell* GetTabletCellOrThrow(const TTabletCellId& id);

    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet, TTabletId);
private:
    class TTabletCellBundleTypeHandler;
    class TTabletCellTypeHandler;
    class TTabletTypeHandler;
    class TImpl;

    TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TTabletManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
