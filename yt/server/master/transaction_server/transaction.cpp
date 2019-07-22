#include "transaction.h"

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/serialize.h>

#include <yt/server/master/chunk_server/chunk_owner_base.h>

#include <yt/server/master/security_server/account.h>
#include <yt/server/master/security_server/subject.h>

#include <yt/core/misc/string.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NTransactionServer {

using namespace NYTree;
using namespace NYson;
using namespace NCellMaster;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

void TTransaction::TExportEntry::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Object);
    Persist(context, DestinationCellTag);
}

////////////////////////////////////////////////////////////////////////////////

TTransaction::TTransaction(TTransactionId id)
    : TTransactionBase(id)
    , Parent_(nullptr)
    , StartTime_(TInstant::Zero())
    , Acd_(this)
{ }

void TTransaction::Save(NCellMaster::TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);
    TTransactionBase::Save(context);

    using NYT::Save;
    Save(context, GetPersistentState());
    Save(context, Timeout_);
    Save(context, Title_);
    Save(context, SecondaryCellTags_);
    Save(context, NestedTransactions_);
    Save(context, Parent_);
    Save(context, StartTime_);
    Save(context, StagedObjects_);
    Save(context, ExportedObjects_);
    Save(context, ImportedObjects_);
    Save(context, LockedNodes_);
    Save(context, Locks_);
    Save(context, BranchedNodes_);
    Save(context, StagedNodes_);
    Save(context, AccountResourceUsage_);
    Save(context, Acd_);
    Save(context, PrerequisiteTransactions_);
    Save(context, DependentTransactions_);
    Save(context, Deadline_);
}

void TTransaction::Load(NCellMaster::TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);
    TTransactionBase::Load(context);

    using NYT::Load;
    Load(context, State_);
    Load(context, Timeout_);
    Load(context, Title_);
    Load(context, SecondaryCellTags_);
    Load(context, NestedTransactions_);
    Load(context, Parent_);
    Load(context, StartTime_);
    Load(context, StagedObjects_);
    Load(context, ExportedObjects_);
    Load(context, ImportedObjects_);
    Load(context, LockedNodes_);
    Load(context, Locks_);
    Load(context, BranchedNodes_);
    Load(context, StagedNodes_);
    Load(context, AccountResourceUsage_);
    Load(context, Acd_);
    // COMPAT(shakurov)
    if (context.GetVersion() < EMasterSnapshotVersion::RemoveTTransactionSystem) {
        Load<bool>(context); // drop System_
    }
    Load(context, PrerequisiteTransactions_);
    Load(context, DependentTransactions_);
    // COMPAT(ignat)
    if (context.GetVersion() >= EMasterSnapshotVersion::PersistTransactionDeadline) {
        Load(context, Deadline_);
    }
}

void TTransaction::RecomputeResourceUsage()
{
    AccountResourceUsage_.clear();

    for (auto* node : BranchedNodes_) {
        AddNodeResourceUsage(node, false);
    }
    for (auto* node : StagedNodes_) {
        AddNodeResourceUsage(node, true);
    }
}

void TTransaction::AddNodeResourceUsage(const NCypressServer::TCypressNode* node, bool staged)
{
    if (node->IsExternal()) {
        return;
    }

    auto* account = node->GetAccount();
    AccountResourceUsage_[account] += node->GetDeltaResourceUsage();
}

bool TTransaction::IsDescendantOf(TTransaction* transaction) const
{
    YT_VERIFY(transaction);
    for (auto* current = GetParent(); current; current = current->GetParent()) {
        if (current == transaction) {
            return true;
        }
    }
    return false;
}

namespace {

template <class TFluent>
void DumpTransaction(TFluent fluent, const TTransaction* transaction, bool dumpParents)
{
    auto customAttributes = CreateEphemeralAttributes();
    auto copyCustomAttribute = [&] (const TString& key) {
        if (!transaction->GetAttributes()) {
            return;
        }
        const auto& attributeMap = transaction->GetAttributes()->Attributes();
        auto it = attributeMap.find(key);
        if (it == attributeMap.end()) {
            return;
        }
        customAttributes->SetYson(it->first, it->second);
    };
    copyCustomAttribute("operation_id");
    copyCustomAttribute("operation_title");

    fluent
        .BeginMap()
            .Item("id").Value(transaction->GetId())
            .Item("start_time").Value(transaction->GetStartTime())
            .Item("owner").Value(transaction->Acd().GetOwner()->GetName())
            .DoIf(transaction->GetTimeout().operator bool(), [&] (TFluentMap fluent) {
                fluent
                    .Item("timeout").Value(*transaction->GetTimeout());
            })
            .DoIf(transaction->GetTitle().operator bool(), [&] (TFluentMap fluent) {
                fluent
                    .Item("title").Value(*transaction->GetTitle());
            }).DoIf(dumpParents, [&] (auto fluent) {
                std::vector<TTransaction*> parents;
                auto* parent = transaction->GetParent();
                while (parent) {
                    parents.push_back(parent);
                    parent = parent->GetParent();
                }
                fluent.Item("parents").DoListFor(parents, [&] (auto fluent, auto* parent) {
                    fluent
                        .Item().Do([&] (auto fluent) {
                            DumpTransaction(fluent, parent, false);
                        });
                });
            })
        .EndMap();
}

} // namespace

TYsonString TTransaction::GetErrorDescription() const
{
    return BuildYsonStringFluently()
        .Do([&] (auto fluent) {
            DumpTransaction(fluent, this, true);
        });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer

