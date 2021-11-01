#include "table_node.h"
#include "private.h"
#include "master_table_schema.h"
#include "table_collocation.h"
#include "table_manager.h"

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/master/chunk_server/chunk_list.h>

#include <yt/yt/server/master/tablet_server/tablet.h>
#include <yt/yt/server/master/tablet_server/tablet_cell_bundle.h>

namespace NYT::NTableServer {

using namespace NCellMaster;
using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NCypressServer;
using namespace NObjectServer;
using namespace NSecurityServer;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletServer;
using namespace NTransactionClient;
using namespace NTransactionServer;
using namespace NYTree;
using namespace NYson;
using namespace NCrypto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TableServerLogger;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESchemaSerializationMethod,
    (Schema)
    (TableIdWithSameSchema)
);

////////////////////////////////////////////////////////////////////////////////

void TDynamicTableLock::Persist(const NCellMaster::TPersistenceContext& context)
{
    using ::NYT::Persist;
    Persist(context, PendingTabletCount);
}

////////////////////////////////////////////////////////////////////////////////

TTableNode::TDynamicTableAttributes::TDynamicTableAttributes()
    : TabletBalancerConfig(New<TTabletBalancerConfig>())
{ }

void TTableNode::TDynamicTableAttributes::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Atomicity);
    Save(context, CommitOrdering);
    Save(context, UpstreamReplicaId);
    Save(context, LastCommitTimestamp);
    Save(context, TabletCountByState);
    Save(context, Tablets);
    Save(context, InMemoryMode);
    Save(context, TabletErrorCount);
    Save(context, ForcedCompactionRevision);
    Save(context, ForcedStoreCompactionRevision);
    Save(context, ForcedHunkCompactionRevision);
    Save(context, Dynamic);
    Save(context, MountPath);
    Save(context, ExternalTabletResourceUsage);
    Save(context, ExpectedTabletState);
    Save(context, LastMountTransactionId);
    Save(context, TabletCountByExpectedState);
    Save(context, ActualTabletState);
    Save(context, PrimaryLastMountTransactionId);
    Save(context, CurrentMountTransactionId);
    Save(context, *TabletBalancerConfig);
    Save(context, DynamicTableLocks);
    Save(context, UnconfirmedDynamicTableLockCount);
    Save(context, EnableDynamicStoreRead);
    Save(context, MountedWithEnabledDynamicStoreRead);
    Save(context, TabletStatistics);
    Save(context, ProfilingMode);
    Save(context, ProfilingTag);
    Save(context, EnableDetailedProfiling);
    Save(context, BackupState);
    Save(context, TabletCountByBackupState);
    Save(context, AggregatedTabletBackupState);
}

void TTableNode::TDynamicTableAttributes::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, Atomicity);
    Load(context, CommitOrdering);
    Load(context, UpstreamReplicaId);
    Load(context, LastCommitTimestamp);
    Load(context, TabletCountByState);
    Load(context, Tablets);
    Load(context, InMemoryMode);
    Load(context, TabletErrorCount);
    Load(context, ForcedCompactionRevision);
    if (context.GetVersion() >= EMasterReign::HunkCompaction) {
        Load(context, ForcedStoreCompactionRevision);
        Load(context, ForcedHunkCompactionRevision);
    }
    Load(context, Dynamic);
    Load(context, MountPath);
    // COMPAT(ifsmirnov)
    if (context.GetVersion() < EMasterReign::BundleQuotas) {
        auto resources = Load<NSecurityServer::TClusterResources>(context);
        ExternalTabletResourceUsage = NSecurityServer::ConvertToTabletResources(resources);
    } else {
        Load(context, ExternalTabletResourceUsage);
    }
    Load(context, ExpectedTabletState);
    Load(context, LastMountTransactionId);
    Load(context, TabletCountByExpectedState);
    Load(context, ActualTabletState);
    Load(context, PrimaryLastMountTransactionId);
    Load(context, CurrentMountTransactionId);
    Load(context, *TabletBalancerConfig);
    Load(context, DynamicTableLocks);
    Load(context, UnconfirmedDynamicTableLockCount);
    Load(context, EnableDynamicStoreRead);
    Load(context, MountedWithEnabledDynamicStoreRead);
    Load(context, TabletStatistics);
    // COMPAT(akozhikhov)
    if (context.GetVersion() >= EMasterReign::MakeProfilingModeAnInheritedAttribute_20_3) {
        Load(context, ProfilingMode);
        Load(context, ProfilingTag);
    }
    // COMPAT(akozhikhov)
    if (context.GetVersion() >= EMasterReign::FlagForDetailedProfiling) {
        Load(context, EnableDetailedProfiling);
    }
    // COMPAT(ifsmirnov)
    if (context.GetVersion() >= EMasterReign::BackupsInitial) {
        Load(context, BackupState);
        Load(context, TabletCountByBackupState);
        Load(context, AggregatedTabletBackupState);
    }
}

#define FOR_EACH_COPYABLE_ATTRIBUTE(XX) \
    XX(Dynamic) \
    XX(Atomicity) \
    XX(CommitOrdering) \
    XX(InMemoryMode) \
    XX(UpstreamReplicaId) \
    XX(LastCommitTimestamp) \
    XX(EnableDynamicStoreRead) \
    XX(ProfilingMode) \
    XX(ProfilingTag) \
    XX(EnableDetailedProfiling) \


void TTableNode::TDynamicTableAttributes::CopyFrom(const TDynamicTableAttributes* other)
{
    #define XX(attr) attr = other->attr;
    FOR_EACH_COPYABLE_ATTRIBUTE(XX)
    #undef XX

    TabletBalancerConfig = CloneYsonSerializable(other->TabletBalancerConfig);
}

void TTableNode::TDynamicTableAttributes::BeginCopy(TBeginCopyContext* context) const
{
    using NYT::Save;

    #define XX(attr) Save(*context, attr);
    FOR_EACH_COPYABLE_ATTRIBUTE(XX)
    #undef XX

    Save(*context, ConvertToYsonString(TabletBalancerConfig));
}

void TTableNode::TDynamicTableAttributes::EndCopy(TEndCopyContext* context)
{
    using NYT::Load;

    #define XX(attr) Load(*context, attr);
    FOR_EACH_COPYABLE_ATTRIBUTE(XX)
    #undef XX

    TabletBalancerConfig = ConvertTo<TTabletBalancerConfigPtr>(Load<TYsonString>(*context));
}

#undef FOR_EACH_COPYABLE_ATTRIBUTE

////////////////////////////////////////////////////////////////////////////////

TTableNode::TTableNode(TVersionedNodeId id)
    : TChunkOwnerBase(id)
{
    if (IsTrunk()) {
        SetOptimizeFor(EOptimizeFor::Lookup);
    }
}

TTableNode* TTableNode::GetTrunkNode()
{
    return TrunkNode_->As<TTableNode>();
}

const TTableNode* TTableNode::GetTrunkNode() const
{
    return TrunkNode_->As<TTableNode>();
}

void TTableNode::EndUpload(const TEndUploadContext& context)
{
    if (IsDynamic()) {
        if (SchemaMode_ != context.SchemaMode ||
            *GetSchema()->AsTableSchema() != *context.Schema->AsTableSchema())
        {
            YT_LOG_ALERT("Schema of a dynamic table changed during end upload (TableId: %v, TransactionId: %v, "
                "OriginalSchemaMode: %v, NewSchemaMode: %v, OriginalSchema: %v, NewSchema: %v)",
                GetId(),
                GetTransaction()->GetId(),
                SchemaMode_,
                context.SchemaMode,
                GetSchema()->AsTableSchema(),
                context.Schema->AsTableSchema());
        }
    }

    SchemaMode_ = context.SchemaMode;

    const auto& tableManager = context.Bootstrap->GetTableManager();
    tableManager->SetTableSchema(this, context.Schema);

    if (context.OptimizeFor) {
        OptimizeFor_.Set(*context.OptimizeFor);
    }
    TChunkOwnerBase::EndUpload(context);
}

TClusterResources TTableNode::GetDeltaResourceUsage() const
{
    return TChunkOwnerBase::GetDeltaResourceUsage();
}

TClusterResources TTableNode::GetTotalResourceUsage() const
{
    return TChunkOwnerBase::GetTotalResourceUsage();
}

TTabletResources TTableNode::GetTabletResourceUsage() const
{
    int tabletCount = 0;
    i64 tabletStaticMemory = 0;

    if (IsTrunk()) {
        tabletCount = Tablets().size();
        for (const auto* tablet : Tablets()) {
            if (tablet->GetState() != ETabletState::Unmounted) {
                tabletStaticMemory += tablet->GetTabletStaticMemorySize();
            }
        }
    }

    auto resourceUsage = TTabletResources()
        .SetTabletCount(tabletCount)
        .SetTabletStaticMemory(tabletStaticMemory);

    return resourceUsage + GetExternalTabletResourceUsage();
}

TDetailedMasterMemory TTableNode::GetDetailedMasterMemoryUsage() const
{
    auto result = TChunkOwnerBase::GetDetailedMasterMemoryUsage();
    result[EMasterMemoryType::Tablets] += GetTabletMasterMemoryUsage();
    return result;
}

void TTableNode::RecomputeTabletMasterMemoryUsage()
{
    i64 masterMemoryUsage = 0;
    for (const auto* tablet : Tablets()) {
        masterMemoryUsage += tablet->GetTabletMasterMemoryUsage();
    }
    SetTabletMasterMemoryUsage(masterMemoryUsage);
}

bool TTableNode::IsSorted() const
{
    return GetSchema()->AsTableSchema()->IsSorted();
}

bool TTableNode::IsUniqueKeys() const
{
    return GetSchema()->AsTableSchema()->IsUniqueKeys();
}

bool TTableNode::IsReplicated() const
{
    return GetType() == EObjectType::ReplicatedTable;
}

bool TTableNode::IsPhysicallySorted() const
{
    return IsSorted() && !IsReplicated();
}

ETabletState TTableNode::GetTabletState() const
{
    if (GetLastMountTransactionId()) {
        return ETabletState::Transient;
    }

    if (!IsDynamic()) {
        return ETabletState::None;
    }

    return GetActualTabletState();
}

ETabletState TTableNode::ComputeActualTabletState() const
{
    auto* trunkNode = GetTrunkNode();
    if (trunkNode->Tablets().empty()) {
        return ETabletState::None;
    }
    for (auto state : TEnumTraits<ETabletState>::GetDomainValues()) {
        if (trunkNode->TabletCountByState().IsDomainValue(state)) {
            if (std::ssize(trunkNode->Tablets()) == trunkNode->TabletCountByState()[state]) {
                return state;
            }
        }
    }
    return ETabletState::Mixed;
}

void TTableNode::Save(NCellMaster::TSaveContext& context) const
{
    TChunkOwnerBase::Save(context);

    using NYT::Save;
    SaveTableSchema(context);
    Save(context, SchemaMode_);
    Save(context, OptimizeFor_);
    Save(context, RetainedTimestamp_);
    Save(context, UnflushedTimestamp_);
    Save(context, TabletCellBundle_);
    Save(context, ReplicationCollocation_);
    TUniquePtrSerializer<>::Save(context, DynamicTableAttributes_);
}

void TTableNode::Load(NCellMaster::TLoadContext& context)
{
    TChunkOwnerBase::Load(context);

    using NYT::Load;
    LoadTableSchema(context);
    Load(context, SchemaMode_);
    Load(context, OptimizeFor_);
    Load(context, RetainedTimestamp_);
    Load(context, UnflushedTimestamp_);
    Load(context, TabletCellBundle_);
    // COMPAT(akozhikhov).
    if (context.GetVersion() >= EMasterReign::TableCollocation) {
        Load(context, ReplicationCollocation_);
    }
    TUniquePtrSerializer<>::Load(context, DynamicTableAttributes_);
}

void TTableNode::LoadTableSchema(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    // COMPAT(shakurov)
    if (context.GetVersion() < EMasterReign::TrueTableSchemaObjects) {
        // NB: using the table manager (which is global by its nature) only works
        // here for compat-loading a snapshot. Loading this way during EndCopy
        // would cause trouble. Luckily, there's no need for that.
        const auto& tableManager = context.GetBootstrap()->GetTableManager();
        auto* emptyMasterTableSchema = tableManager->GetOrCreateEmptyMasterTableSchema();
        const auto& emptyTableSchema = *emptyMasterTableSchema->AsTableSchema();

        switch (Load<ESchemaSerializationMethod>(context)) {
            case ESchemaSerializationMethod::Schema: {
                auto tableSchema = Load<TTableSchema>(context);
                if (tableSchema == emptyTableSchema) {
                    Schema_ = emptyMasterTableSchema;
                } else {
                    auto tableSchemaId = ReplaceTypeInId(Id_, EObjectType::MasterTableSchema);
                    auto versionedIdHash = NObjectClient::TDirectVersionedObjectIdHash()(GetVersionedId());
                    tableSchemaId.Parts32[0] = static_cast<ui32>(versionedIdHash);
                    Schema_ = tableManager->CreateMasterTableSchemaUnsafely(tableSchemaId, tableSchema);
                }
                YT_VERIFY(context.LoadedSchemas().emplace(GetVersionedId(), Schema_).second);

                break;
            }
            case ESchemaSerializationMethod::TableIdWithSameSchema: {
                auto previousTableId = Load<TVersionedObjectId>(context);
                auto it = context.LoadedSchemas().find(previousTableId);
                YT_VERIFY(it != context.LoadedSchemas().end());
                Schema_ = it->second;
                break;
            }
            default:
                YT_ABORT();
        }

        const auto& objectManager = context.GetBootstrap()->GetObjectManager();
        objectManager->RefObject(Schema_);
    } else {
        Load(context, Schema_);
    }
}

void TTableNode::SaveTableSchema(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, Schema_);
}

std::pair<TTableNode::TTabletListIterator, TTableNode::TTabletListIterator> TTableNode::GetIntersectingTablets(
    const TLegacyOwningKey& minKey,
    const TLegacyOwningKey& maxKey)
{
    auto* trunkNode = GetTrunkNode();

    auto beginIt = std::upper_bound(
        trunkNode->Tablets().cbegin(),
        trunkNode->Tablets().cend(),
        minKey,
        [] (const TLegacyOwningKey& key, const TTablet* tablet) {
            return key < tablet->GetPivotKey();
        });

    if (beginIt != trunkNode->Tablets().cbegin()) {
        --beginIt;
    }

    auto endIt = beginIt;
    while (endIt != trunkNode->Tablets().cend() && maxKey >= (*endIt)->GetPivotKey()) {
        ++endIt;
    }

    return std::make_pair(beginIt, endIt);
}

bool TTableNode::IsDynamic() const
{
    return GetTrunkNode()->GetDynamic();
}

bool TTableNode::IsEmpty() const
{
    return ComputeTotalStatistics().chunk_count() == 0;
}

bool TTableNode::IsLogicallyEmpty() const
{
    const auto* chunkList = GetChunkList();
    YT_VERIFY(chunkList);
    return chunkList->Statistics().LogicalRowCount == 0;
}

TTimestamp TTableNode::GetCurrentUnflushedTimestamp(
    TTimestamp latestTimestamp) const
{
    // COMPAT(savrus) Consider saved value only for non-trunk nodes.
    return !IsTrunk() && UnflushedTimestamp_ != NullTimestamp
        ? UnflushedTimestamp_
        : CalculateUnflushedTimestamp(latestTimestamp);
}

TTimestamp TTableNode::GetCurrentRetainedTimestamp() const
{
    // COMPAT(savrus) Consider saved value only for non-trunk nodes.
    return !IsTrunk() && RetainedTimestamp_ != NullTimestamp
        ? RetainedTimestamp_
        : CalculateRetainedTimestamp();
}

TTimestamp TTableNode::CalculateUnflushedTimestamp(
    TTimestamp latestTimestamp) const
{
    auto* trunkNode = GetTrunkNode();
    if (!trunkNode->IsDynamic()) {
        return NullTimestamp;
    }

    auto result = MaxTimestamp;
    for (const auto* tablet : trunkNode->Tablets()) {
        auto timestamp = tablet->GetState() != ETabletState::Unmounted
            ? static_cast<TTimestamp>(tablet->NodeStatistics().unflushed_timestamp())
            : latestTimestamp;
        result = std::min(result, timestamp);
    }
    return result;
}

TTimestamp TTableNode::CalculateRetainedTimestamp() const
{
    auto* trunkNode = GetTrunkNode();
    if (!trunkNode->IsDynamic()) {
        return NullTimestamp;
    }

    auto result = MinTimestamp;
    for (const auto* tablet : trunkNode->Tablets()) {
        auto timestamp = tablet->GetRetainedTimestamp();
        result = std::max(result, timestamp);
    }
    return result;
}

TMasterTableSchema* TTableNode::GetSchema() const
{
    return Schema_;
}

void TTableNode::SetSchema(TMasterTableSchema* schema)
{
    Schema_ = schema;
}

void TTableNode::UpdateExpectedTabletState(ETabletState state)
{
    auto current = GetExpectedTabletState();

    YT_ASSERT(current == ETabletState::Frozen ||
        current == ETabletState::Mounted ||
        current == ETabletState::Unmounted);
    YT_ASSERT(state == ETabletState::Frozen ||
        state == ETabletState::Mounted);

    if (state == ETabletState::Mounted ||
        (state == ETabletState::Frozen && current != ETabletState::Mounted))
    {
        SetExpectedTabletState(state);
    }
}

void TTableNode::ValidateNoCurrentMountTransaction(TStringBuf message) const
{
    const auto* trunkTable = GetTrunkNode();
    auto transactionId = trunkTable->GetCurrentMountTransactionId();
    if (transactionId) {
        THROW_ERROR_EXCEPTION(NTabletClient::EErrorCode::InvalidTabletState, "%v since node is locked by mount-unmount operation", message)
            << TErrorAttribute("current_mount_transaction_id", transactionId);
    }
}

void TTableNode::LockCurrentMountTransaction(TTransactionId transactionId)
{
    YT_ASSERT(!static_cast<bool>(GetCurrentMountTransactionId()));
    SetCurrentMountTransactionId(transactionId);
}

void TTableNode::UnlockCurrentMountTransaction(TTransactionId transactionId)
{
    if (GetCurrentMountTransactionId() == transactionId) {
        SetCurrentMountTransactionId(TTransactionId());
    }
}

void TTableNode::ValidateTabletStateFixed(TStringBuf message) const
{
    ValidateNoCurrentMountTransaction(message);

    const auto* trunkTable = GetTrunkNode();
    auto transactionId = trunkTable->GetLastMountTransactionId();
    if (transactionId) {
        THROW_ERROR_EXCEPTION(NTabletClient::EErrorCode::InvalidTabletState, "%v since some tablets are in transient state", message)
            << TErrorAttribute("last_mount_transaction_id", transactionId)
            << TErrorAttribute("expected_tablet_state", trunkTable->GetExpectedTabletState());
    }
}

void TTableNode::ValidateExpectedTabletState(TStringBuf message, bool allowFrozen) const
{
    ValidateTabletStateFixed(message);

    const auto* trunkTable = GetTrunkNode();
    auto state = trunkTable->GetExpectedTabletState();
    if (!(state == ETabletState::Unmounted || (allowFrozen && state == ETabletState::Frozen))) {
        THROW_ERROR_EXCEPTION(NTabletClient::EErrorCode::InvalidTabletState, "%v since not all tablets are %v",
            message,
            allowFrozen ? "frozen or unmounted" : "unmounted")
            << TErrorAttribute("actual_tablet_state", trunkTable->GetActualTabletState())
            << TErrorAttribute("expected_tablet_state", trunkTable->GetExpectedTabletState());
    }
}

void TTableNode::ValidateAllTabletsFrozenOrUnmounted(TStringBuf message) const
{
    ValidateExpectedTabletState(message, true);
}

void TTableNode::ValidateAllTabletsUnmounted(TStringBuf message) const
{
    ValidateExpectedTabletState(message, false);
}

void TTableNode::ValidateNotBackup(TStringBuf message) const
{
    if (GetBackupState() == ETableBackupState::BackupCompleted) {
        THROW_ERROR_EXCEPTION(NTabletClient::EErrorCode::InvalidBackupState, "%v", message);
    }
}

std::optional<bool> TTableNode::GetEnableTabletBalancer() const
{
    return TabletBalancerConfig()->EnableAutoReshard
        ? std::nullopt
        : std::make_optional(false);
}

void TTableNode::SetEnableTabletBalancer(std::optional<bool> value)
{
    MutableTabletBalancerConfig()->EnableAutoReshard = value.value_or(true);
}

std::optional<i64> TTableNode::GetMinTabletSize() const
{
    return TabletBalancerConfig()->MinTabletSize;
}

void TTableNode::SetMinTabletSize(std::optional<i64> value)
{
    MutableTabletBalancerConfig()->SetMinTabletSize(value);
}

std::optional<i64> TTableNode::GetMaxTabletSize() const
{
    return TabletBalancerConfig()->MaxTabletSize;
}

void TTableNode::SetMaxTabletSize(std::optional<i64> value)
{
    MutableTabletBalancerConfig()->SetMaxTabletSize(value);
}

std::optional<i64> TTableNode::GetDesiredTabletSize() const
{
    return TabletBalancerConfig()->DesiredTabletSize;
}

void TTableNode::SetDesiredTabletSize(std::optional<i64> value)
{
    MutableTabletBalancerConfig()->SetDesiredTabletSize(value);
}

std::optional<int> TTableNode::GetDesiredTabletCount() const
{
    return TabletBalancerConfig()->DesiredTabletCount;
}

void TTableNode::SetDesiredTabletCount(std::optional<int> value)
{
    MutableTabletBalancerConfig()->DesiredTabletCount = value;
}

void TTableNode::AddDynamicTableLock(
    TTransactionId transactionId,
    TTimestamp timestamp,
    int pendingTabletCount)
{
    YT_VERIFY(MutableDynamicTableLocks().emplace(
        transactionId, TDynamicTableLock{timestamp, pendingTabletCount}).second);
    SetUnconfirmedDynamicTableLockCount(GetUnconfirmedDynamicTableLockCount() + 1);
}

void TTableNode::ConfirmDynamicTableLock(TTransactionId transactionId)
{
    if (auto it = MutableDynamicTableLocks().find(transactionId)) {
        YT_VERIFY(it->second.PendingTabletCount > 0);
        --it->second.PendingTabletCount;
        if (it->second.PendingTabletCount == 0) {
            SetUnconfirmedDynamicTableLockCount(GetUnconfirmedDynamicTableLockCount() - 1);
        }
    }
}

void TTableNode::RemoveDynamicTableLock(TTransactionId transactionId)
{
    if (auto it = MutableDynamicTableLocks().find(transactionId)) {
        if (it->second.PendingTabletCount > 0) {
            SetUnconfirmedDynamicTableLockCount(GetUnconfirmedDynamicTableLockCount() - 1);
        }
        MutableDynamicTableLocks().erase(it);
    }
}

DEFINE_EXTRA_PROPERTY_HOLDER(TTableNode, TTableNode::TDynamicTableAttributes, DynamicTableAttributes);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableServer

