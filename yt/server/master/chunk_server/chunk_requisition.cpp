#include "chunk_requisition.h"

#include "chunk_manager.h"
#include "medium.h"
#include "private.h"

#include <yt/server/master/cell_master/automaton.h>
#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/serialize.h>

#include <yt/server/master/security_server/security_manager.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/core/misc/serialize.h>

namespace NYT::NChunkServer {

using namespace NYTree;

using NYT::ToProto;
using NYT::FromProto;

static const auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TReplicationPolicy& TReplicationPolicy::operator|=(const TReplicationPolicy& rhs)
{
    if (this == &rhs) {
        return *this;
    }

    SetReplicationFactor(std::max(GetReplicationFactor(), rhs.GetReplicationFactor()));
    SetDataPartsOnly(GetDataPartsOnly() && rhs.GetDataPartsOnly());

    return *this;
}

void TReplicationPolicy::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, ReplicationFactor_);
    Save(context, DataPartsOnly_);
}

void TReplicationPolicy::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    ReplicationFactor_ = Load<decltype(ReplicationFactor_)>(context);
    DataPartsOnly_ = Load<decltype(DataPartsOnly_)>(context);
}

void FormatValue(TStringBuilderBase* builder, TReplicationPolicy policy, TStringBuf /*spec*/)
{
    builder->AppendFormat("{ReplicationFactor: %v, DataPartsOnly: %v}",
        policy.GetReplicationFactor(),
        policy.GetDataPartsOnly());
}

TString ToString(TReplicationPolicy policy)
{
    return ToStringViaBuilder(policy);
}

void Serialize(const TReplicationPolicy& policy, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("replication_factor").Value(policy.GetReplicationFactor())
            .Item("data_parts_only").Value(policy.GetDataPartsOnly())
        .EndMap();
}

void Deserialize(TReplicationPolicy& policy, NYTree::INodePtr node)
{
    auto map = node->AsMap();
    policy.SetReplicationFactor(map->GetChild("replication_factor")->AsInt64()->GetValue());
    policy.SetDataPartsOnly(map->GetChild("data_parts_only")->AsBoolean()->GetValue());
}

////////////////////////////////////////////////////////////////////////////////

TChunkReplication::TEntry::TEntry(int mediumIndex, TReplicationPolicy policy)
    : MediumIndex_(static_cast<ui16>(mediumIndex))
    , Policy_(policy)
{ }

void TChunkReplication::TEntry::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, MediumIndex_);
    Save(context, Policy_);
}

void TChunkReplication::TEntry::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, MediumIndex_);
    Load(context, Policy_);
}

////////////////////////////////////////////////////////////////////////////////

void TChunkReplication::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Entries_);
    Save(context, Vital_);
}

void TChunkReplication::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    // COMPAT(shakurov)
    if (context.GetVersion() < NCellMaster::EMasterSnapshotVersion::InTChunkReplicationReplaceArrayWithSmallVector) {
        constexpr auto oldMaxMediumCount = 7;
        std::array<TReplicationPolicy, oldMaxMediumCount> oldReplicationPolicies{};
        Load(context, oldReplicationPolicies);
        for (auto mediumIndex = 0; mediumIndex < oldMaxMediumCount; ++mediumIndex) {
            if (auto& policy = oldReplicationPolicies[mediumIndex]) {
                Insert(mediumIndex, policy);
            }
        }
    } else {
        Load(context, Entries_);
    }

    Load(context, Vital_);
}

void TChunkReplication::ClearEntries()
{
    Entries_.clear();
}

bool TChunkReplication::IsValid() const
{
    for (const auto& entry : Entries_) {
        if (entry.Policy() && !entry.Policy().GetDataPartsOnly()) {
            // At least one medium has complete data.
            return true;
        }
    }

    return false;
}

void FormatValue(TStringBuilderBase* builder, const TChunkReplication& replication, TStringBuf /*spec*/)
{
    return builder->AppendFormat(
        "{Vital: %v, Media: {%v}}",
        replication.GetVital(),
        MakeFormattableView(
            replication,
            [&] (TStringBuilderBase* aBuilder, const TChunkReplication::TEntry& entry) {
                aBuilder->AppendFormat("%v: %v", entry.GetMediumIndex(), entry.Policy());
            }));
}

TString ToString(const TChunkReplication& replication)
{
    return ToStringViaBuilder(replication);
}

////////////////////////////////////////////////////////////////////////////////

TSerializableChunkReplication::TSerializableChunkReplication(
    const TChunkReplication& replication,
    const TChunkManagerPtr& chunkManager)
{
    for (const auto& entry : replication) {
        if (entry.Policy()) {
            auto* medium = chunkManager->GetMediumByIndex(entry.GetMediumIndex());
            YT_VERIFY(IsObjectAlive(medium));
            YT_VERIFY(Entries_.emplace(medium->GetName(), entry.Policy()).second);
        }
    }
}

void TSerializableChunkReplication::ToChunkReplication(
    TChunkReplication* replication,
    const TChunkManagerPtr& chunkManager)
{
    replication->ClearEntries();

    for (const auto& pair : Entries_) {
        auto* medium = chunkManager->GetMediumByNameOrThrow(pair.first);
        auto mediumIndex = medium->GetIndex();
        replication->Set(mediumIndex, pair.second);
    }
}

void TSerializableChunkReplication::Serialize(NYson::IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer)
        .Value(Entries_);
}

void TSerializableChunkReplication::Deserialize(INodePtr node)
{
    YT_VERIFY(node);

    Entries_ = ConvertTo<std::map<TString, TReplicationPolicy>>(node);
}

void Serialize(const TSerializableChunkReplication& serializer, NYson::IYsonConsumer* consumer)
{
    serializer.Serialize(consumer);
}

void Deserialize(TSerializableChunkReplication& serializer, INodePtr node)
{
    serializer.Deserialize(node);
}

////////////////////////////////////////////////////////////////////////////////

void ValidateReplicationFactor(int replicationFactor)
{
    if (replicationFactor != 0 && // Zero is a special - and permitted - case.
        (replicationFactor < NChunkClient::MinReplicationFactor ||
         replicationFactor > NChunkClient::MaxReplicationFactor))
    {
        THROW_ERROR_EXCEPTION("Replication factor %v is out of range [%v,%v]",
            replicationFactor,
            NChunkClient::MinReplicationFactor,
            NChunkClient::MaxReplicationFactor);
    }
}

void ValidateChunkReplication(
    const TChunkManagerPtr& chunkManager,
    const TChunkReplication& replication,
    std::optional<int> primaryMediumIndex)
{
    if (!replication.IsValid()) {
        THROW_ERROR_EXCEPTION(
            "At least one medium should store replicas (including parity parts); "
            "configuring otherwise would result in a data loss");
    }

    for (const auto& entry : replication) {
        auto* medium = chunkManager->FindMediumByIndex(entry.GetMediumIndex());
        YT_VERIFY(IsObjectAlive(medium));

        if (entry.Policy() && medium->GetCache()) {
            THROW_ERROR_EXCEPTION("Cache medium %Qv cannot be configured explicitly",
                medium->GetName());
        }
    }

    if (primaryMediumIndex) {
        const auto* primaryMedium = chunkManager->GetMediumByIndex(*primaryMediumIndex);
        const auto& policy = replication.Get(*primaryMediumIndex);
        if (!policy) {
            THROW_ERROR_EXCEPTION("Medium %Qv is not configured and cannot be made primary",
                primaryMedium->GetName());
        }
        if (policy.GetDataPartsOnly()) {
            THROW_ERROR_EXCEPTION("Medium %Qv stores no parity parts and cannot be made primary",
                                  primaryMedium->GetName());
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void TRequisitionEntry::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, Account);
    Save(context, MediumIndex);
    Save(context, ReplicationPolicy);
    Save(context, Committed);
}

void TRequisitionEntry::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, Account);
    Load(context, MediumIndex);
    Load(context, ReplicationPolicy);
    Load(context, Committed);
}

void FormatValue(TStringBuilderBase* builder, const TRequisitionEntry& entry, TStringBuf /*spec*/)
{
    return builder->AppendFormat(
        "{AccountId: %v, MediumIndex: %v, ReplicationPolicy: %v, Committed: %v}",
        entry.Account->GetId(),
        entry.MediumIndex,
        entry.ReplicationPolicy,
        entry.Committed);
}

TString ToString(const TRequisitionEntry& entry)
{
    return ToStringViaBuilder(entry);
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TReqUpdateChunkRequisition::TChunkRequisition* protoRequisition, const TChunkRequisition& requisition)
{
    protoRequisition->set_vital(requisition.GetVital());
    for (const auto& entry : requisition) {
        auto* protoEntry = protoRequisition->add_entries();
        ToProto(protoEntry->mutable_account_id(), entry.Account->GetId());
        protoEntry->set_medium_index(entry.MediumIndex);
        protoEntry->set_replication_factor(entry.ReplicationPolicy.GetReplicationFactor());
        protoEntry->set_data_parts_only(entry.ReplicationPolicy.GetDataPartsOnly());
        protoEntry->set_committed(entry.Committed);
    }
}

void FromProto(
    TChunkRequisition* requisition,
    const NProto::TReqUpdateChunkRequisition::TChunkRequisition& protoRequisition,
    const NSecurityServer::TSecurityManagerPtr& securityManager)
{
    requisition->SetVital(protoRequisition.vital());

    for (const auto& entry : protoRequisition.entries()) {
        auto* account = securityManager->FindAccount(FromProto<NSecurityServer::TAccountId>(entry.account_id()));

        // NB: an account may be removed between replicator sending a requisition and chunk manager receiving it.
        if (!IsObjectAlive(account)) {
            continue;
        }

        requisition->AddEntry(
            account,
            entry.medium_index(),
            TReplicationPolicy(entry.replication_factor(), entry.data_parts_only()),
            entry.committed());
    }
}

void TChunkRequisition::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    YT_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));
    Save(context, Entries_);

    Save(context, Vital_);
}

void TChunkRequisition::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, Entries_);
    YT_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));

    Load(context, Vital_);
}

void TChunkRequisition::ForceReplicationFactor(int replicationFactor)
{
    YT_ASSERT(replicationFactor > 0);

    for (auto& entry : Entries_) {
        auto& replicationPolicy = entry.ReplicationPolicy;
        YT_VERIFY(replicationPolicy);
        replicationPolicy.SetReplicationFactor(replicationFactor);
    }
}

TChunkRequisition& TChunkRequisition::operator|=(const TChunkRequisition& rhs)
{
    if (this == &rhs) {
        return *this;
    }

    Vital_ = Vital_ || rhs.Vital_;
    AggregateEntries(rhs.Entries_);

    return *this;
}

void TChunkRequisition::AggregateWith(
    const TChunkReplication& replication,
    NSecurityServer::TAccount* account,
    bool committed)
{
    YT_ASSERT(account);

    Vital_ = Vital_ || replication.GetVital();

    for (const auto& entry : replication) {
        if (entry.Policy()) {
            Entries_.emplace_back(account, entry.GetMediumIndex(), entry.Policy(), committed);
        }
    }

    NormalizeEntries();
}

TChunkReplication TChunkRequisition::ToReplication() const
{
    TChunkReplication result;
    result.SetVital(Vital_);

    auto foundCommitted = false;
    for (const auto& entry : Entries_) {
        if (entry.Committed) {
            result.Aggregate(entry.MediumIndex, entry.ReplicationPolicy);
            foundCommitted = true;
        }
    }

    if (!foundCommitted) {
        for (const auto& entry : Entries_) {
            result.Aggregate(entry.MediumIndex, entry.ReplicationPolicy);
        }
    }

    return result;
}

void TChunkRequisition::AggregateEntries(const TEntries& newEntries)
{
    if (newEntries.empty()) {
        return;
    }

    Entries_.reserve(Entries_.size() + newEntries.size());
    Entries_.insert(Entries_.end(), newEntries.begin(), newEntries.end());

    NormalizeEntries();
}

void TChunkRequisition::NormalizeEntries()
{
    Entries_.erase(
        std::remove_if(
            Entries_.begin(),
            Entries_.end(),
            [] (const TRequisitionEntry& entry) { return !entry.ReplicationPolicy; }),
        Entries_.end());

    std::sort(Entries_.begin(), Entries_.end());
    Entries_.erase(std::unique(Entries_.begin(), Entries_.end()), Entries_.end());

    if (Entries_.empty()) {
        return;
    }

    // Second, merge entries by "account, medium, committed" triplets.
    auto mergeRangeBegin = Entries_.begin();
    for (auto mergeRangeIt = mergeRangeBegin + 1; mergeRangeIt != Entries_.end(); ++mergeRangeIt) {
        YT_ASSERT(
            (mergeRangeBegin->Account == mergeRangeIt->Account) ==
            (mergeRangeBegin->Account->GetId() == mergeRangeIt->Account->GetId()));

        if (mergeRangeBegin->Account == mergeRangeIt->Account &&
            mergeRangeBegin->MediumIndex == mergeRangeIt->MediumIndex &&
            mergeRangeBegin->Committed == mergeRangeIt->Committed)
        {
            mergeRangeBegin->ReplicationPolicy |= mergeRangeIt->ReplicationPolicy;
        } else {
            ++mergeRangeBegin;
            // Avoid self-assignment.
            if (mergeRangeBegin != mergeRangeIt) {
                *mergeRangeBegin = *mergeRangeIt;
            }
        }
    }
    Entries_.erase(mergeRangeBegin + 1, Entries_.end());

    YT_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));
}

void TChunkRequisition::AddEntry(
    NSecurityServer::TAccount* account,
    int mediumIndex,
    TReplicationPolicy replicationPolicy,
    bool committed)
{
    YT_ASSERT(account);
    Entries_.emplace_back(account, mediumIndex, replicationPolicy, committed);
}

void FormatValue(TStringBuilderBase* builder, const TChunkRequisition& requisition, TStringBuf /*spec*/)
{
    builder->AppendFormat(
        "{Vital: %v, Entries: {%v}}",
        requisition.GetVital(),
        MakeFormattableView(
            requisition,
            [] (TStringBuilderBase* builder, const TRequisitionEntry& entry) {
                FormatValue(builder, entry);
            }));
}

TString ToString(const TChunkRequisition& requisition)
{
    return ToStringViaBuilder(requisition);
}

////////////////////////////////////////////////////////////////////////////////


TSerializableChunkRequisition::TSerializableChunkRequisition(
    const TChunkRequisition& requisition,
    const TChunkManagerPtr& chunkManager)
{
    Entries_.reserve(requisition.GetEntryCount());
    for (const auto& entry : requisition) {
        auto* account = entry.Account;
        if (!IsObjectAlive(account)) {
            continue;
        }

        auto* medium = chunkManager->GetMediumByIndex(entry.MediumIndex);

        Entries_.push_back(TEntry{account->GetName(), medium->GetName(), entry.ReplicationPolicy, entry.Committed});
    }
}

void TSerializableChunkRequisition::Serialize(NYson::IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer).
        Value(Entries_);
}

void TSerializableChunkRequisition::Deserialize(NYTree::INodePtr node)
{
    YT_VERIFY(node);

    Entries_ = ConvertTo<std::vector<TEntry>>(node);
}

void Serialize(const TSerializableChunkRequisition::TEntry& entry, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("account").Value(entry.Account)
            .Item("medium").Value(entry.Medium)
            .Item("replication_policy").Value(entry.ReplicationPolicy)
            .Item("committed").Value(entry.Committed)
        .EndMap();

}

void Deserialize(TSerializableChunkRequisition::TEntry& entry, NYTree::INodePtr node)
{
    auto map = node->AsMap();
    entry.Account = map->GetChild("account")->AsString()->GetValue();
    entry.Medium = map->GetChild("medium")->AsString()->GetValue();
    Deserialize(entry.ReplicationPolicy, map->GetChild("replication_policy"));
    entry.Committed = map->GetChild("committed")->AsBoolean()->GetValue();
}

void Serialize(const TSerializableChunkRequisition& serializer, NYson::IYsonConsumer* consumer)
{
    serializer.Serialize(consumer);
}

void Deserialize(TSerializableChunkRequisition& serializer, NYTree::INodePtr node)
{
    serializer.Deserialize(node);
}

////////////////////////////////////////////////////////////////////////////////

void TChunkRequisitionRegistry::TIndexedItem::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, RefCount);
    Save(context, Requisition);
    // Replication is not persisted as it's restored from Requisition.
}

void TChunkRequisitionRegistry::TIndexedItem::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, RefCount);
    Load(context, Requisition);
    Replication = Requisition.ToReplication();
}

////////////////////////////////////////////////////////////////////////////////

void TChunkRequisitionRegistry::Clear()
{
    NextIndex_ = EmptyChunkRequisitionIndex;
    IndexToItem_.clear();
    RequisitionToIndex_.clear();
}

void TChunkRequisitionRegistry::EnsureBuiltinRequisitionsInitialized(
    NSecurityServer::TAccount* chunkWiseAccountingMigrationAccount,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    if (IndexToItem_.contains(EmptyChunkRequisitionIndex)) {
        YT_VERIFY(IndexToItem_.contains(MigrationChunkRequisitionIndex));
        YT_VERIFY(IndexToItem_.contains(MigrationRF2ChunkRequisitionIndex));
        YT_VERIFY(IndexToItem_.contains(MigrationErasureChunkRequisitionIndex));

        return;
    }

    YT_VERIFY(Insert(TChunkRequisition(), objectManager) == EmptyChunkRequisitionIndex);

    // When migrating to chunk-wise accounting, assume all chunks belong to a
    // special migration account.
    TChunkRequisition defaultRequisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(NChunkClient::DefaultReplicationFactor, false /* dataPartsOnly */),
        true /* committed */);
    YT_VERIFY(Insert(defaultRequisition, objectManager) == MigrationChunkRequisitionIndex);

    TChunkRequisition rf2Requisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(2 /*replicationFactor*/, false /* dataPartsOnly */),
        true /* committed */);
    YT_VERIFY(Insert(rf2Requisition, objectManager) == MigrationRF2ChunkRequisitionIndex);

    TChunkRequisition defaultErasureRequisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(1 /*replicationFactor*/, false /* dataPartsOnly */),
        true /* committed */);
    YT_VERIFY(Insert(defaultErasureRequisition, objectManager) == MigrationErasureChunkRequisitionIndex);

    FakeRefBuiltinRequisitions();
}

void TChunkRequisitionRegistry::FakeRefBuiltinRequisitions() {
    // Fake reference - always keep builtin requisitions.
    Ref(EmptyChunkRequisitionIndex);
    Ref(MigrationChunkRequisitionIndex);
    Ref(MigrationRF2ChunkRequisitionIndex);
    Ref(MigrationErasureChunkRequisitionIndex);
}

void TChunkRequisitionRegistry::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    using TSortedIndexItem = std::pair<TChunkRequisitionIndex, TIndexedItem>;

    std::vector<TSortedIndexItem> sortedIndex(IndexToItem_.begin(), IndexToItem_.end());
    std::sort(
        sortedIndex.begin(),
        sortedIndex.end(),
        [] (const TSortedIndexItem& lhs, const TSortedIndexItem& rhs) {
            return lhs.first < rhs.first;
        });
    Save(context, sortedIndex);
    Save(context, NextIndex_);
}

void TChunkRequisitionRegistry::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    using TSortedIndexItem = std::pair<TChunkRequisitionIndex, TIndexedItem>;

    auto sortedIndex =  Load<std::vector<TSortedIndexItem>>(context);

    IndexToItem_.reserve(sortedIndex.size());
    RequisitionToIndex_.reserve(sortedIndex.size());

    for (const auto& pair : sortedIndex) {
        IndexToItem_.emplace(pair.first, pair.second);
        RequisitionToIndex_.emplace(pair.second.Requisition, pair.first);
    }

    YT_VERIFY(IndexToItem_.contains(EmptyChunkRequisitionIndex));
    YT_VERIFY(IndexToItem_.contains(MigrationChunkRequisitionIndex));
    YT_VERIFY(IndexToItem_.contains(MigrationRF2ChunkRequisitionIndex));
    YT_VERIFY(IndexToItem_.contains(MigrationErasureChunkRequisitionIndex));

    Load(context, NextIndex_);

    YT_VERIFY(!IndexToItem_.contains(NextIndex_));
}

TChunkRequisitionIndex TChunkRequisitionRegistry::GetOrCreate(
    const TChunkRequisition& requisition,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto it = RequisitionToIndex_.find(requisition);
    if (it != RequisitionToIndex_.end()) {
        YT_ASSERT(IndexToItem_.find(it->second) != IndexToItem_.end());
        return it->second;
    }

    return Insert(requisition, objectManager);
}

std::optional<TChunkRequisitionIndex> TChunkRequisitionRegistry::Find(
    const TChunkRequisition& requisition) const
{
    auto it = RequisitionToIndex_.find(requisition);
    return it != RequisitionToIndex_.end() ? it->second : std::optional<TChunkRequisitionIndex>();
}


TChunkRequisitionIndex TChunkRequisitionRegistry::Insert(
    const TChunkRequisition& requisition,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto index = GenerateIndex();

    TIndexedItem item;
    item.Requisition = requisition;
    item.Replication = requisition.ToReplication();
    item.RefCount = 0; // This is ok, Ref()/Unref() will be called soon.
    YT_VERIFY(IndexToItem_.emplace(index, item).second);
    YT_VERIFY(RequisitionToIndex_.emplace(requisition, index).second);

    for (const auto& entry : requisition) {
        objectManager->WeakRefObject(entry.Account);
    }

    YT_LOG_DEBUG("Requisition created (RequisitionIndex: %v, Requisition: %v)",
        index,
        requisition);

    return index;
}

void TChunkRequisitionRegistry::Erase(
    TChunkRequisitionIndex index,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto it = IndexToItem_.find(index);
    // Copy: we need the requisition to weak-unref accounts, and we need
    // accounts to hash requisitions when erasing them.
    auto requisition = it->second.Requisition;

    YT_LOG_DEBUG("Requisition removed (RequisitionIndex: %v, Requisition: %v)",
        index,
        requisition);

    YT_VERIFY(RequisitionToIndex_.erase(requisition) == 1);
    IndexToItem_.erase(it);

    for (const auto& entry : requisition) {
        objectManager->WeakUnrefObject(entry.Account);
    }
}

void TChunkRequisitionRegistry::Ref(TChunkRequisitionIndex index)
{
    auto it = IndexToItem_.find(index);
    YT_VERIFY(it != IndexToItem_.end());
    ++it->second.RefCount;
    YT_LOG_TRACE("Requisition referenced (RequisitionIndex: %v, RefCount: %v)",
        index,
        it->second.RefCount);
}

void TChunkRequisitionRegistry::Unref(
    TChunkRequisitionIndex index,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto it = IndexToItem_.find(index);
    YT_VERIFY(it != IndexToItem_.end());
    YT_VERIFY(it->second.RefCount != 0);
    --it->second.RefCount;

    YT_LOG_TRACE("Requisition unreferenced (RequisitionIndex: %v, RefCount: %v)",
        index,
        it->second.RefCount);

    if (it->second.RefCount == 0) {
        Erase(index, objectManager);
    }
}

void TChunkRequisitionRegistry::Serialize(
    NYson::IYsonConsumer* consumer,
    const TChunkManagerPtr& chunkManager) const
{
    using TSortedIndexItem = std::pair<TChunkRequisitionIndex, TIndexedItem>;

    std::vector<TSortedIndexItem> sortedIndex(IndexToItem_.begin(), IndexToItem_.end());
    std::sort(
        sortedIndex.begin(),
        sortedIndex.end(),
        [] (const TSortedIndexItem& lhs, const TSortedIndexItem& rhs) {
            return lhs.first < rhs.first;
        });

    BuildYsonFluently(consumer)
        .DoMapFor(sortedIndex, [&] (TFluentMap fluent, const TSortedIndexItem& pair) {
            auto index = pair.first;
            const auto& item = pair.second;
            const auto& requisition = item.Requisition;
            auto refCount = item.RefCount;
            TSerializableChunkRequisition requisitionSerializer(requisition, chunkManager);
            fluent
                .Item(ToString(index))
                .BeginMap()
                    .Item("ref_counter").Value(refCount)
                    .Item("vital").Value(requisition.GetVital())
                    .Item("entries").Value(requisitionSerializer)
                .EndMap();
        });
}

////////////////////////////////////////////////////////////////////////////////

TSerializableChunkRequisitionRegistry::TSerializableChunkRequisitionRegistry(
    const TChunkManagerPtr& chunkManager)
    : ChunkManager_(chunkManager)
{ }

void TSerializableChunkRequisitionRegistry::Serialize(NYson::IYsonConsumer* consumer) const
{
    const auto* registry = ChunkManager_->GetChunkRequisitionRegistry();
    registry->Serialize(consumer, ChunkManager_);
}

void Serialize(const TSerializableChunkRequisitionRegistry& serializer, NYson::IYsonConsumer* consumer)
{
    serializer.Serialize(consumer);
}

////////////////////////////////////////////////////////////////////////////////

const TChunkRequisition& TEphemeralRequisitionRegistry::GetRequisition(TChunkRequisitionIndex index) const
{
    auto it = IndexToRequisition_.find(index);
    YT_VERIFY(it != IndexToRequisition_.end());
    return it->second;
}

TChunkRequisitionIndex TEphemeralRequisitionRegistry::GetOrCreateIndex(const TChunkRequisition& requisition)
{
    auto it = RequisitionToIndex_.find(requisition);
    if (it != RequisitionToIndex_.end()) {
        YT_ASSERT(IndexToRequisition_.find(it->second) != IndexToRequisition_.end());
        return it->second;
    }

    return Insert(requisition);
}

void TEphemeralRequisitionRegistry::Clear()
{
    IndexToRequisition_.clear();
    RequisitionToIndex_.clear();
    NextIndex_ = 0;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
