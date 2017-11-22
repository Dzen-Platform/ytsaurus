#include "chunk_requisition.h"

#include "chunk_manager.h"
#include "medium.h"

#include <yt/server/cell_master/automaton.h>
#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/security_manager.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/core/misc/serialize.h>

namespace NYT {
namespace NChunkServer {

using namespace NYTree;

using NYT::ToProto;
using NYT::FromProto;

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

void FormatValue(TStringBuilder* builder, TReplicationPolicy policy, const TStringBuf& /*spec*/)
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

TChunkReplication::TChunkReplication(bool clearForCombining)
{
    if (clearForCombining) {
        for (auto& policy : MediumReplicationPolicies) {
            policy.SetDataPartsOnly(true);
        }
    }
}

void TChunkReplication::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, MediumReplicationPolicies);
    Save(context, Vital_);
}

void TChunkReplication::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, MediumReplicationPolicies);
    Load(context, Vital_);
}

TChunkReplication& TChunkReplication::operator|=(const TChunkReplication& rhs)
{
    if (this == &rhs) {
        return *this;
    }

    SetVital(GetVital() || rhs.GetVital());

    for (int i = 0; i < MaxMediumCount; ++i) {
        (*this)[i] |= rhs[i];
    }

    return *this;
}

bool TChunkReplication::IsValid() const
{
    for (const auto& policy : MediumReplicationPolicies) {
        if (policy && !policy.GetDataPartsOnly()) {
            // At least one medium has complete data.
            return true;
        }
    }

    return false;
}

void FormatValue(TStringBuilder* builder, TChunkReplication replication, const TStringBuf& /*spec*/)
{
    // We want to accompany medium policies with medium indexes.
    using TIndexPolicyPair = std::pair<int, TReplicationPolicy>;

    SmallVector<TIndexPolicyPair, MaxMediumCount> filteredPolicies;
    int mediumIndex = 0;
    for (const auto& policy : replication) {
        if (policy) {
            filteredPolicies.emplace_back(mediumIndex, policy);
        }
        ++mediumIndex;
    }

    return builder->AppendFormat(
        "{Vital: %v, Media: {%v}}",
        replication.GetVital(),
        MakeFormattableRange(
            filteredPolicies,
            [&] (TStringBuilder* aBuilder, const TIndexPolicyPair& pair) {
                aBuilder->AppendFormat("%v: %v", pair.first, pair.second);
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
    for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        const auto& policy = replication[mediumIndex];
        if (policy) {
            auto* medium = chunkManager->GetMediumByIndex(mediumIndex);
            YCHECK(MediumReplicationPolicies_.emplace(medium->GetName(), policy).second);
        }
    }
}

void TSerializableChunkReplication::ToChunkReplication(
    TChunkReplication* replication,
    const TChunkManagerPtr& chunkManager)
{
    for (auto& policy : *replication) {
        policy.Clear();
    }

    for (const auto& pair : MediumReplicationPolicies_) {
        auto* medium = chunkManager->GetMediumByNameOrThrow(pair.first);
        auto mediumIndex = medium->GetIndex();
        auto& policy = (*replication)[mediumIndex];
        policy = pair.second;
    }
}

void TSerializableChunkReplication::Serialize(NYson::IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer)
        .Value(MediumReplicationPolicies_);
}

void TSerializableChunkReplication::Deserialize(INodePtr node)
{
    YCHECK(node);

    MediumReplicationPolicies_ = ConvertTo<std::map<TString, TReplicationPolicy>>(node);
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
    int primaryMediumIndex)
{
    if (!replication.IsValid()) {
        THROW_ERROR_EXCEPTION(
            "At least one medium should store replicas (including parity parts); "
            "configuring otherwise would result in a data loss");
    }

    for (int index = 0; index < MaxMediumCount; ++index) {
        const auto* medium = chunkManager->FindMediumByIndex(index);
        if (!medium) {
            continue;
        }

        const auto& policy = replication[index];
        if (policy && medium->GetCache()) {
            THROW_ERROR_EXCEPTION("Cache medium %Qv cannot be configured explicitly",
                medium->GetName());
        }
    }

    const auto* primaryMedium = chunkManager->GetMediumByIndex(primaryMediumIndex);
    const auto& primaryMediumPolicy = replication[primaryMediumIndex];
    if (!primaryMediumPolicy) {
        THROW_ERROR_EXCEPTION("Medium %Qv is not configured and cannot be made primary",
            primaryMedium->GetName());
    }
    if (primaryMediumPolicy.GetDataPartsOnly()) {
        THROW_ERROR_EXCEPTION("Medium %Qv stores no parity parts and cannot be made primary",
            primaryMedium->GetName());
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

void FormatValue(TStringBuilder* builder, const TRequisitionEntry& entry, const TStringBuf& /*spec*/)
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

    Y_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));
    Save(context, Entries_);

    Save(context, Vital_);
}

void TChunkRequisition::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, Entries_);
    Y_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));

    Load(context, Vital_);
}

void TChunkRequisition::ForceReplicationFactor(int replicationFactor)
{
    for (auto& entry : Entries_) {
        auto& replicationPolicy = entry.ReplicationPolicy;
        if (replicationPolicy) {
            replicationPolicy.SetReplicationFactor(replicationFactor);
        }
    }
}

TChunkRequisition& TChunkRequisition::operator|=(const TChunkRequisition& rhs)
{
    if (this == &rhs) {
        return *this;
    }

    Vital_ = Vital_ || rhs.Vital_;
    CombineEntries(rhs.Entries_);

    return *this;
}

void TChunkRequisition::CombineWith(
    const TChunkReplication& replication,
    NSecurityServer::TAccount* account,
    bool committed)
{
    Y_ASSERT(account);

    Vital_ = Vital_ || replication.GetVital();

    for (auto mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        const auto& policy = replication[mediumIndex];
        if (policy) {
            Entries_.emplace_back(account, mediumIndex, policy, committed);
        }
    }

    NormalizeEntries();
}

TChunkReplication TChunkRequisition::ToReplication() const
{
    TChunkReplication result(true /* clearForCombining */);
    result.SetVital(Vital_);

    auto foundCommitted = false;
    for (const auto& entry : Entries_) {
        if (entry.Committed) {
            result[entry.MediumIndex] |= entry.ReplicationPolicy;
            foundCommitted = true;
        }
    }

    if (!foundCommitted) {
        for (const auto& entry : Entries_) {
            result[entry.MediumIndex] |= entry.ReplicationPolicy;
        }
    }

    return result;
}

void TChunkRequisition::CombineEntries(const TEntries& newEntries)
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
        Y_ASSERT(
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

    Y_ASSERT(std::is_sorted(Entries_.begin(), Entries_.end()));
}

void TChunkRequisition::AddEntry(
    NSecurityServer::TAccount* account,
    int mediumIndex,
    TReplicationPolicy replicationPolicy,
    bool committed)
{
    Y_ASSERT(account);
    Entries_.emplace_back(account, mediumIndex, replicationPolicy, committed);
}

void FormatValue(TStringBuilder* builder, const TChunkRequisition& requisition, const TStringBuf& /*spec*/)
{
    builder->AppendFormat(
        "{Vital: %v, Entries: {%v}}",
        requisition.GetVital(),
        MakeFormattableRange(
            requisition,
            [] (TStringBuilder* builder, const TRequisitionEntry& entry) {
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
    YCHECK(node);

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
    Save(context, Requisition);
    // Replication is not persisted as it's restored from Requisition.
    // RefCount is not persisted as it's recalculated by chunk manager.
}

void TChunkRequisitionRegistry::TIndexedItem::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
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
    if (IndexToItem_.has(EmptyChunkRequisitionIndex)) {
        YCHECK(IndexToItem_.has(MigrationChunkRequisitionIndex));
        YCHECK(IndexToItem_.has(MigrationRF2ChunkRequisitionIndex));
        YCHECK(IndexToItem_.has(MigrationErasureChunkRequisitionIndex));

        return;
    }

    YCHECK(Insert(TChunkRequisition(), objectManager) == EmptyChunkRequisitionIndex);
    Ref(EmptyChunkRequisitionIndex); // Fake reference - always keep the empty requisition.

    // When migrating to chunk-wise accounting, assume all chunks belong to a
    // special migration account.
    TChunkRequisition defaultRequisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(NChunkClient::DefaultReplicationFactor, false /* dataPartsOnly */),
        true /* committed */);
    YCHECK(Insert(defaultRequisition, objectManager) == MigrationChunkRequisitionIndex);
    Ref(MigrationChunkRequisitionIndex); // Fake reference - always keep the migration requisition.

    TChunkRequisition rf2Requisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(2 /*replicationFactor*/, false /* dataPartsOnly */),
        true /* committed */);
    YCHECK(Insert(rf2Requisition, objectManager) == MigrationRF2ChunkRequisitionIndex);
    Ref(MigrationRF2ChunkRequisitionIndex);

    TChunkRequisition defaultErasureRequisition(
        chunkWiseAccountingMigrationAccount,
        DefaultStoreMediumIndex,
        TReplicationPolicy(1 /*replicationFactor*/, false /* dataPartsOnly */),
        true /* committed */);
    YCHECK(Insert(defaultErasureRequisition, objectManager) == MigrationErasureChunkRequisitionIndex);
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

    std::vector<TSortedIndexItem> sortedIndex;
    Load(context, sortedIndex);

    IndexToItem_.reserve(sortedIndex.size());
    RequisitionToIndex_.reserve(sortedIndex.size());

    for (const auto& pair : sortedIndex) {
        IndexToItem_.emplace(pair.first, pair.second);
        RequisitionToIndex_.emplace(pair.second.Requisition, pair.first);
    }

    YCHECK(IndexToItem_.has(EmptyChunkRequisitionIndex));
    YCHECK(IndexToItem_.has(MigrationChunkRequisitionIndex));
    YCHECK(IndexToItem_.has(MigrationRF2ChunkRequisitionIndex));
    YCHECK(IndexToItem_.has(MigrationErasureChunkRequisitionIndex));

    Load(context, NextIndex_);

    YCHECK(!IndexToItem_.has(NextIndex_));
}

TChunkRequisitionIndex TChunkRequisitionRegistry::GetIndex(
    const TChunkRequisition& requisition,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto it = RequisitionToIndex_.find(requisition);
    if (it != RequisitionToIndex_.end()) {
        Y_ASSERT(IndexToItem_.find(it->second) != IndexToItem_.end());
        return it->second;
    }

    return Insert(requisition, objectManager);
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
    YCHECK(IndexToItem_.emplace(index, item).second);
    YCHECK(RequisitionToIndex_.emplace(requisition, index).second);

    for (const auto& entry : requisition) {
        objectManager->WeakRefObject(entry.Account);
    }

    return index;
}

void TChunkRequisitionRegistry::Erase(
    TChunkRequisitionIndex index,
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    auto it = IndexToItem_.find(index);
    const auto& requisition = it->second.Requisition;

    for (const auto& entry : requisition) {
        objectManager->WeakUnrefObject(entry.Account);
    }

    RequisitionToIndex_.erase(requisition);
    IndexToItem_.erase(it);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
