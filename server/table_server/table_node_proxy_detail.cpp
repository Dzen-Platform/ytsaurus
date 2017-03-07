#include "table_node_proxy_detail.h"
#include "private.h"
#include "table_node.h"
#include "replicated_table_node.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/chunk_server/chunk.h>
#include <yt/server/chunk_server/chunk_list.h>

#include <yt/server/node_tracker_server/node_directory_builder.h>

#include <yt/server/tablet_server/tablet.h>
#include <yt/server/tablet_server/tablet_cell.h>
#include <yt/server/tablet_server/table_replica.h>
#include <yt/server/tablet_server/tablet_manager.h>

#include <yt/server/object_server/object_manager.h>

#include <yt/ytlib/chunk_client/read_limit.h>

#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/serialize.h>
#include <yt/core/misc/string.h>

#include <yt/core/ypath/token.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/tree_builder.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/yson/async_consumer.h>

namespace NYT {
namespace NTableServer {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NCypressServer;
using namespace NObjectServer;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NTableClient;
using namespace NTransactionServer;
using namespace NTabletServer;
using namespace NNodeTrackerServer;

using NChunkClient::TChannel;
using NChunkClient::TReadLimit;

////////////////////////////////////////////////////////////////////////////////

TTableNodeProxy::TTableNodeProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TTableNode* trunkNode)
    : TBase(
        bootstrap,
        metadata,
        transaction,
        trunkNode)
{ }

void TTableNodeProxy::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TBase::ListSystemAttributes(descriptors);

    const auto* table = GetThisImpl();
    bool isDynamic = table->IsDynamic();
    bool isSorted = table->IsSorted();

    descriptors->push_back(TAttributeDescriptor("chunk_row_count"));
    descriptors->push_back(TAttributeDescriptor("row_count")
        .SetPresent(!isDynamic));
    // TODO(savrus) remove "unmerged_row_count" in 20.0
    descriptors->push_back(TAttributeDescriptor("unmerged_row_count")
        .SetPresent(isDynamic && isSorted));
    descriptors->push_back("sorted");
    descriptors->push_back(TAttributeDescriptor("key_columns")
        .SetReplicated(true));
    descriptors->push_back(TAttributeDescriptor("schema")
        .SetReplicated(true));
    descriptors->push_back(TAttributeDescriptor("sorted_by")
        .SetPresent(table->TableSchema().IsSorted()));
    descriptors->push_back("dynamic");
    descriptors->push_back(TAttributeDescriptor("tablet_count")
        .SetPresent(isDynamic));
    descriptors->push_back(TAttributeDescriptor("tablet_state")
        .SetPresent(isDynamic)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("last_commit_timestamp")
        .SetPresent(isDynamic && isSorted));
    descriptors->push_back(TAttributeDescriptor("tablets")
        .SetPresent(isDynamic)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("tablet_count_by_state")
        .SetPresent(isDynamic)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("pivot_keys")
        .SetPresent(isDynamic && isSorted)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("retained_timestamp")
        .SetPresent(isDynamic && isSorted));
    descriptors->push_back(TAttributeDescriptor("unflushed_timestamp")
        .SetPresent(isDynamic && isSorted));
    descriptors->push_back(TAttributeDescriptor("tablet_statistics")
        .SetPresent(isDynamic)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("tablet_cell_bundle")
        .SetPresent(table->GetTabletCellBundle() != nullptr));
    descriptors->push_back("atomicity");
    descriptors->push_back(TAttributeDescriptor("commit_ordering")
        .SetPresent(!table->IsSorted()));
    descriptors->push_back(TAttributeDescriptor("optimize_for")
        .SetCustom(true));
    descriptors->push_back(TAttributeDescriptor("schema_mode"));
    descriptors->push_back(TAttributeDescriptor("chunk_writer")
        .SetCustom(true));
}

bool TTableNodeProxy::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    const auto* table = GetThisImpl();
    bool isDynamic = table->IsDynamic();
    bool isSorted = table->IsSorted();

    const auto* trunkTable = table->GetTrunkNode();
    auto statistics = table->ComputeTotalStatistics();

    const auto& tabletManager = Bootstrap_->GetTabletManager();

    if (key == "chunk_row_count") {
        BuildYsonFluently(consumer)
            .Value(statistics.row_count());
        return true;
    }

    if (key == "row_count" && !isDynamic) {
        BuildYsonFluently(consumer)
            .Value(statistics.row_count());
        return true;
    }

    if (key == "unmerged_row_count" && isDynamic && isSorted) {
        BuildYsonFluently(consumer)
            .Value(statistics.row_count());
        return true;
    }

    if (key == "sorted") {
        BuildYsonFluently(consumer)
            .Value(table->TableSchema().IsSorted());
        return true;
    }

    if (key == "key_columns") {
        BuildYsonFluently(consumer)
            .Value(table->TableSchema().GetKeyColumns());
        return true;
    }

    if (key == "schema") {
        BuildYsonFluently(consumer)
            .Value(table->TableSchema());
        return true;
    }

    if (key == "schema_mode") {
        BuildYsonFluently(consumer)
            .Value(table->GetSchemaMode());
        return true;
    }

    if (key == "sorted_by" && table->TableSchema().IsSorted()) {
        BuildYsonFluently(consumer)
            .Value(table->TableSchema().GetKeyColumns());
        return true;
    }

    if (key == "dynamic") {
        BuildYsonFluently(consumer)
            .Value(table->IsDynamic());
        return true;
    }

    if (key == "tablet_count" && isDynamic) {
        BuildYsonFluently(consumer)
            .Value(trunkTable->Tablets().size());
        return true;
    }

    if (key == "tablet_count_by_state" && isDynamic) {
        TEnumIndexedVector<int, ETabletState> counts;
        for (const auto& tablet : trunkTable->Tablets()) {
            ++counts[tablet->GetState()];
        }
        BuildYsonFluently(consumer)
            .DoMapFor(TEnumTraits<ETabletState>::GetDomainValues(), [&] (TFluentMap fluent, ETabletState state) {
                fluent.Item(FormatEnum(state)).Value(counts[state]);
            });
        return true;
    }

    if (key == "tablet_state" && isDynamic) {
        BuildYsonFluently(consumer)
            .Value(trunkTable->GetTabletState());
        return true;
    }

    if (key == "last_commit_timestamp" && isDynamic && isSorted) {
        BuildYsonFluently(consumer)
            .Value(trunkTable->GetLastCommitTimestamp());
        return true;
    }

    if (key == "tablets" && isDynamic) {
        BuildYsonFluently(consumer)
            .DoListFor(trunkTable->Tablets(), [&] (TFluentList fluent, TTablet* tablet) {
                auto* cell = tablet->GetCell();
                fluent
                    .Item().BeginMap()
                        .Item("index").Value(tablet->GetIndex())
                        .Item("performance_counters").Value(tablet->PerformanceCounters())
                        .DoIf(table->IsSorted(), [&] (TFluentMap fluent) {
                            fluent
                                .Item("pivot_key").Value(tablet->GetPivotKey());
                        })
                        .DoIf(!table->IsPhysicallySorted(), [&] (TFluentMap fluent) {
                            const auto* chunkList = tablet->GetChunkList();
                            fluent
                                .Item("trimmed_row_count").Value(tablet->GetTrimmedRowCount())
                                .Item("flushed_row_count").Value(chunkList->Statistics().LogicalRowCount);
                        })
                        .Item("state").Value(tablet->GetState())
                        .Item("last_commit_timestamp").Value(tablet->NodeStatistics().last_commit_timestamp())
                        .Item("statistics").Value(tabletManager->GetTabletStatistics(tablet))
                        .Item("tablet_id").Value(tablet->GetId())
                        .DoIf(cell, [&] (TFluentMap fluent) {
                            fluent.Item("cell_id").Value(cell->GetId());
                        })
                    .EndMap();
            });
        return true;
    }

    if (key == "pivot_keys" && isDynamic && isSorted) {
        BuildYsonFluently(consumer)
            .DoListFor(trunkTable->Tablets(), [&] (TFluentList fluent, TTablet* tablet) {
                fluent
                    .Item().Value(tablet->GetPivotKey());
            });
        return true;
    }

    if (key == "retained_timestamp" && isDynamic && isSorted) {
        BuildYsonFluently(consumer)
            .Value(table->GetCurrentRetainedTimestamp());
        return true;
    }

    if (key == "unflushed_timestamp" && isDynamic && isSorted) {
        BuildYsonFluently(consumer)
            .Value(table->GetCurrentUnflushedTimestamp());
        return true;
    }

    if (key == "tablet_statistics" && isDynamic) {
        TTabletStatistics tabletStatistics;
        for (const auto& tablet : trunkTable->Tablets()) {
            tabletStatistics += tabletManager->GetTabletStatistics(tablet);
        }
        BuildYsonFluently(consumer)
            .Value(tabletStatistics);
        return true;
    }

    if (key == "tablet_cell_bundle" && trunkTable->GetTabletCellBundle()) {
        BuildYsonFluently(consumer)
            .Value(trunkTable->GetTabletCellBundle()->GetName());
        return true;
    }

    if (key == "atomicity") {
        BuildYsonFluently(consumer)
            .Value(trunkTable->GetAtomicity());
        return true;
    }

    if (key == "commit_ordering") {
        BuildYsonFluently(consumer)
            .Value(trunkTable->GetCommitOrdering());
        return true;
    }

    return TBase::GetBuiltinAttribute(key, consumer);
}

void TTableNodeProxy::AlterTable(
    const TNullable<TTableSchema>& newSchema,
    const TNullable<bool>& newDynamic)
{
    auto* table = LockThisImpl();

    if (table->IsReplicated()) {
        THROW_ERROR_EXCEPTION("Cannot alter a replicated table");
    }

    if (newDynamic) {
        ValidateNoTransaction();

        if (*newDynamic && table->IsExternal()) {
            THROW_ERROR_EXCEPTION("External node cannot be a dynamic table");
        }
    }

    if (newSchema && table->IsDynamic() && table->GetTabletState() != ETabletState::Unmounted) {
        THROW_ERROR_EXCEPTION("Cannot change table schema since not all of its tablets are in %Qlv state",
            ETabletState::Unmounted);
    }

    auto dynamic = newDynamic.Get(table->IsDynamic());
    auto schema = newSchema.Get(table->TableSchema());
 
    // NB: Sorted dynamic tables contain unique keys, set this for user.
    if (dynamic && newSchema && newSchema->IsSorted() && !newSchema->GetUniqueKeys()) {
        schema = schema.ToUniqueKeys();
    }

    ValidateTableSchemaUpdate(
        table->TableSchema(),
        schema,
        dynamic,
        table->IsEmpty());

    if (newSchema) {
        table->TableSchema() = std::move(schema);
        table->SetSchemaMode(ETableSchemaMode::Strong);
    }

    if (newDynamic) {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        if (*newDynamic) {
            tabletManager->MakeTableDynamic(table);
        } else {
            tabletManager->MakeTableStatic(table);
        }
    }
}

bool TTableNodeProxy::SetBuiltinAttribute(const Stroka& key, const TYsonString& value)
{
    const auto* table = GetThisImpl();

    if (key == "tablet_cell_bundle") {
        ValidateNoTransaction();

        auto name = ConvertTo<Stroka>(value);
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        auto* cellBundle = tabletManager->GetTabletCellBundleByNameOrThrow(name);

        auto* lockedTable = LockThisImpl();
        tabletManager->SetTabletCellBundle(lockedTable, cellBundle);
        return true;
    }

    if (key == "atomicity") {
        ValidateNoTransaction();

        auto* lockedTable = LockThisImpl();
        if (table->GetTabletState() != ETabletState::Unmounted) {
            THROW_ERROR_EXCEPTION("Cannot change table atomicity mode since not all of its tablets are in %Qlv state",
                ETabletState::Unmounted);
        }

        auto atomicity = ConvertTo<NTransactionClient::EAtomicity>(value);
        lockedTable->SetAtomicity(atomicity);
        return true;
    }

    if (key == "commit_ordering" && !table->IsSorted()) {
        ValidateNoTransaction();

        auto tabletState = table->GetTabletState();
        if (tabletState != ETabletState::Unmounted && tabletState != ETabletState::None) {
            THROW_ERROR_EXCEPTION("Cannot change table commit ordering mode since not all of its tablets are in %Qlv state",
                ETabletState::Unmounted);
        }

        auto* lockedTable = LockThisImpl();
        auto ordering = ConvertTo<NTransactionClient::ECommitOrdering>(value);
        lockedTable->SetCommitOrdering(ordering);
        return true;
    }

    return TBase::SetBuiltinAttribute(key, value);
}

void TTableNodeProxy::ValidateCustomAttributeUpdate(
    const Stroka& key,
    const TYsonString& oldValue,
    const TYsonString& newValue)
{
    if (key == "optimize_for") {
        if (!newValue) {
            ThrowCannotRemoveAttribute(key);
        }
        ConvertTo<EOptimizeFor>(newValue);
        return;
    }

    if (key == "chunk_writer" && newValue) {
        ConvertTo<TTableWriterConfigPtr>(newValue);
        return;
    }

    TBase::ValidateCustomAttributeUpdate(key, oldValue, newValue);
}

void TTableNodeProxy::ValidateFetchParameters(
    const TChannel& channel,
    const std::vector<TReadRange>& ranges)
{
    TChunkOwnerNodeProxy::ValidateFetchParameters(channel, ranges);

    const auto* table = GetThisImpl();
    for (const auto& range : ranges) {
        const auto& lowerLimit = range.LowerLimit();
        const auto& upperLimit = range.UpperLimit();
        if ((upperLimit.HasKey() || lowerLimit.HasKey()) && !table->IsSorted()) {
            THROW_ERROR_EXCEPTION("Key selectors are not supported for unsorted tables");
        }
        if ((upperLimit.HasRowIndex() || lowerLimit.HasRowIndex()) && table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Row index selectors are not supported for dynamic tables");
        }
        if ((upperLimit.HasChunkIndex() || lowerLimit.HasChunkIndex()) && table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Chunk index selectors are not supported for dynamic tables");
        }
        if (upperLimit.HasOffset() || lowerLimit.HasOffset()) {
            THROW_ERROR_EXCEPTION("Offset selectors are not supported for tables");
        }
    }
}


bool TTableNodeProxy::DoInvoke(const IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Mount);
    DISPATCH_YPATH_SERVICE_METHOD(Unmount);
    DISPATCH_YPATH_SERVICE_METHOD(Remount);
    DISPATCH_YPATH_SERVICE_METHOD(Freeze);
    DISPATCH_YPATH_SERVICE_METHOD(Unfreeze);
    DISPATCH_YPATH_SERVICE_METHOD(Reshard);
    DISPATCH_YPATH_SERVICE_METHOD(GetMountInfo);
    DISPATCH_YPATH_SERVICE_METHOD(Alter);
    return TBase::DoInvoke(context);
}

void TTableNodeProxy::ValidateBeginUpload()
{
    TBase::ValidateBeginUpload();

    const auto* table = GetThisImpl();
    if (table->IsDynamic()) {
        THROW_ERROR_EXCEPTION("Cannot upload into a dynamic table");
    }
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Mount)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->last_tablet_index();
    auto cellId = FromProto<TTabletCellId>(request->cell_id());
    bool freeze = request->freeze();

    context->SetRequestInfo(
        "FirstTabletIndex: %v, LastTabletIndex: %v, CellId: %v, Freeze: %v",
        firstTabletIndex,
        lastTabletIndex,
        cellId,
        freeze);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    const auto& tabletManager = Bootstrap_->GetTabletManager();

    TTabletCell* cell = nullptr;
    if (cellId) {
        cell = tabletManager->GetTabletCellOrThrow(cellId);
    }

    auto* table = LockThisImpl();

    tabletManager->MountTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        cell,
        freeze);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Unmount)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->last_tablet_index();
    bool force = request->force();
    context->SetRequestInfo("FirstTabletIndex: %v, LastTabletIndex: %v, Force: %v",
        firstTabletIndex,
        lastTabletIndex,
        force);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    auto* table = LockThisImpl();

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    tabletManager->UnmountTable(
        table,
        force,
        firstTabletIndex,
        lastTabletIndex);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Freeze)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->last_tablet_index();

    context->SetRequestInfo(
        "FirstTabletIndex: %v, LastTabletIndex: %v",
        firstTabletIndex,
        lastTabletIndex);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    auto* table = LockThisImpl();

    tabletManager->FreezeTable(
        table,
        firstTabletIndex,
        lastTabletIndex);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Unfreeze)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->last_tablet_index();
    context->SetRequestInfo("FirstTabletIndex: %v, LastTabletIndex: %v",
        firstTabletIndex,
        lastTabletIndex);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    auto* table = LockThisImpl();

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    tabletManager->UnfreezeTable(
        table,
        firstTabletIndex,
        lastTabletIndex);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Remount)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->first_tablet_index();
    context->SetRequestInfo("FirstTabletIndex: %v, LastTabletIndex: %v",
        firstTabletIndex,
        lastTabletIndex);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    auto* table = LockThisImpl();

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    tabletManager->RemountTable(
        table,
        firstTabletIndex,
        lastTabletIndex);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Reshard)
{
    DeclareMutating();

    int firstTabletIndex = request->first_tablet_index();
    int lastTabletIndex = request->last_tablet_index();
    int tabletCount = request->tablet_count();
    auto pivotKeys = FromProto<std::vector<TOwningKey>>(request->pivot_keys());
    context->SetRequestInfo("FirstTabletIndex: %v, LastTabletIndex: %v, TabletCount: %v",
        firstTabletIndex,
        lastTabletIndex,
        tabletCount);

    ValidateNotExternal();
    ValidateNoTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Mount);

    auto* table = LockThisImpl();

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    tabletManager->ReshardTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        tabletCount,
        pivotKeys);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, GetMountInfo)
{
    DeclareNonMutating();

    context->SetRequestInfo();

    ValidateNotExternal();
    ValidateNoTransaction();

    auto* trunkTable = GetThisImpl();

    ToProto(response->mutable_table_id(), trunkTable->GetId());
    response->set_dynamic(trunkTable->IsDynamic());
    ToProto(response->mutable_schema(), trunkTable->TableSchema());

    yhash_set<TTabletCell*> cells;
    for (auto* tablet : trunkTable->Tablets()) {
        auto* cell = tablet->GetCell();
        auto* protoTablet = response->add_tablets();
        ToProto(protoTablet->mutable_tablet_id(), tablet->GetId());
        protoTablet->set_mount_revision(tablet->GetMountRevision());
        protoTablet->set_state(static_cast<int>(tablet->GetState()));
        ToProto(protoTablet->mutable_pivot_key(), tablet->GetPivotKey());
        if (cell) {
            ToProto(protoTablet->mutable_cell_id(), cell->GetId());
            cells.insert(cell);
        }
    }

    for (const auto* cell : cells) {
        ToProto(response->add_tablet_cells(), cell->GetDescriptor());
    }

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TTableNodeProxy, Alter)
{
    DeclareMutating();

    auto newSchema = request->has_schema()
        ? MakeNullable(FromProto<TTableSchema>(request->schema()))
        : Null;
    auto newDynamic = request->has_dynamic()
        ? MakeNullable(request->dynamic())
        : Null;

    context->SetRequestInfo("Schema: %v, Dynamic: %v",
        newSchema,
        newDynamic);

    AlterTable(newSchema, newDynamic);

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

TReplicatedTableNodeProxy::TReplicatedTableNodeProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TReplicatedTableNode* trunkNode)
    : TTableNodeProxy(
        bootstrap,
        metadata,
        transaction,
        trunkNode)
{ }

void TReplicatedTableNodeProxy::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TTableNodeProxy::ListSystemAttributes(descriptors);

    descriptors->push_back(TAttributeDescriptor("replicas")
        .SetOpaque(true));
}

bool TReplicatedTableNodeProxy::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    const auto* table = GetThisImpl<TReplicatedTableNode>();

    if (key == "replicas") {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        BuildYsonFluently(consumer)
            .DoMapFor(table->Replicas(), [&] (TFluentMap fluent, TTableReplica* replica) {
                auto replicaProxy = objectManager->GetProxy(replica);
                fluent
                    .Item(ToString(replica->GetId()))
                    .BeginMap()
                        .Item("cluster_name").Value(replica->GetClusterName())
                        .Item("replica_path").Value(replica->GetReplicaPath())
                        .Item("state").Value(replica->GetState())
                        .Item("replication_lag_time").Value(replica->ComputeReplicationLagTime())
                    .EndMap();
            });
        return true;
    }

    return TTableNodeProxy::GetBuiltinAttribute(key, consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT


