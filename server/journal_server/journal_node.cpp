#include "journal_node.h"
#include "private.h"
#include "journal_node_proxy.h"
#include "journal_manager.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/config.h>

#include <yt/server/chunk_server/chunk_manager.h>
#include <yt/server/chunk_server/chunk_owner_type_handler.h>

#include <yt/ytlib/journal_client/journal_ypath_proxy.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NJournalServer {

using namespace NCellMaster;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NObjectServer;
using namespace NChunkServer;
using namespace NChunkClient;
using namespace NJournalClient;
using namespace NObjectClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TJournalNode::TJournalNode(const TVersionedNodeId& id)
    : TChunkOwnerBase(id)
    , ReadQuorum_(0)
    , WriteQuorum_(0)
    , Sealed_(true)
{ }

void TJournalNode::Save(NCellMaster::TSaveContext& context) const
{
    TChunkOwnerBase::Save(context);

    using NYT::Save;
    Save(context, ReadQuorum_);
    Save(context, WriteQuorum_);
    Save(context, Sealed_);
}

void TJournalNode::Load(NCellMaster::TLoadContext& context)
{
    TChunkOwnerBase::Load(context);

    using NYT::Load;
    Load(context, ReadQuorum_);
    Load(context, WriteQuorum_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 200) {
        Load(context, Sealed_);
    }
}

void TJournalNode::BeginUpload(NChunkClient::EUpdateMode mode)
{
    TChunkOwnerBase::BeginUpload(mode);

    GetTrunkNode()->Sealed_ = false;
}

TChunk* TJournalNode::GetTrailingChunk() const
{
    if (!ChunkList_) {
        return nullptr;
    }

    if (ChunkList_->Children().empty()) {
        return nullptr;
    }

    return ChunkList_->Children().back()->AsChunk();
}

TJournalNode* TJournalNode::GetTrunkNode()
{
    return TrunkNode_->As<TJournalNode>();
}

const TJournalNode* TJournalNode::GetTrunkNode() const
{
    return TrunkNode_->As<TJournalNode>();
}

bool TJournalNode::GetSealed() const
{
    return GetTrunkNode()->Sealed_;
}

void TJournalNode::SetSealed(bool value)
{
    YCHECK(IsTrunk());
    Sealed_ = value;
}

////////////////////////////////////////////////////////////////////////////////

class TJournalNodeTypeHandler
    : public TChunkOwnerTypeHandler<TJournalNode>
{
public:
    explicit TJournalNodeTypeHandler(TBootstrap* bootstrap)
        : TBase(bootstrap)
    { }

    virtual EObjectType GetObjectType() const override
    {
        return EObjectType::Journal;
    }

    virtual bool IsExternalizable() const override
    {
        return true;
    }

    virtual ENodeType GetNodeType() const override
    {
        return ENodeType::Entity;
    }

    virtual TClusterResources GetTotalResourceUsage(const TCypressNodeBase* node) override
    {
        return TBase::GetTotalResourceUsage(node->GetTrunkNode());
    }

    virtual TClusterResources GetAccountingResourceUsage(const TCypressNodeBase* node) override
    {
        return TBase::GetAccountingResourceUsage(node->GetTrunkNode());
    }

protected:
    typedef TChunkOwnerTypeHandler<TJournalNode> TBase;

    virtual ICypressNodeProxyPtr DoGetProxy(
        TJournalNode* trunkNode,
        TTransaction* transaction) override
    {
        return CreateJournalNodeProxy(
            Bootstrap_,
            &Metadata_,
            transaction,
            trunkNode);
    }

    virtual std::unique_ptr<TJournalNode> DoCreate(
        const TVersionedNodeId& id,
        TCellTag cellTag,
        TTransaction* transaction,
        IAttributeDictionary* attributes) override
    {
        const auto& config = Bootstrap_->GetConfig()->CypressManager;

        // NB: Don't call TBase::InitializeAttributes; take care of all attributes here.

        int replicationFactor = attributes->GetAndRemove<int>("replication_factor", config->DefaultJournalReplicationFactor);
        int readQuorum = attributes->GetAndRemove<int>("read_quorum", config->DefaultJournalReadQuorum);
        int writeQuorum = attributes->GetAndRemove<int>("write_quorum", config->DefaultJournalWriteQuorum);
        auto primaryMediumName = attributes->GetAndRemove<TString>("primary_medium", DefaultStoreMediumName);

        ValidateReplicationFactor(replicationFactor);
        if (readQuorum > replicationFactor) {
            THROW_ERROR_EXCEPTION("\"read_quorum\" cannot be greater than \"replication_factor\"");
        }
        if (writeQuorum > replicationFactor) {
            THROW_ERROR_EXCEPTION("\"write_quorum\" cannot be greater than \"replication_factor\"");
        }
        if (readQuorum + writeQuorum < replicationFactor + 1) {
            THROW_ERROR_EXCEPTION("Read/write quorums are not safe: read_quorum + write_quorum < replication_factor + 1");
        }

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* primaryMedium = chunkManager->GetMediumByNameOrThrow(primaryMediumName);

        auto nodeHolder = TBase::DoCreate(
            id,
            cellTag,
            transaction,
            attributes);
        auto* node = nodeHolder.get();

        node->SetPrimaryMediumIndex(primaryMedium->GetIndex());
        node->Properties()[primaryMedium->GetIndex()].SetReplicationFactor(replicationFactor);
        node->SetReadQuorum(readQuorum);
        node->SetWriteQuorum(writeQuorum);

        return nodeHolder;
    }

    virtual void DoBranch(
        const TJournalNode* originatingNode,
        TJournalNode* branchedNode,
        ELockMode mode) override
    {
        // NB: Don't call TBase::DoBranch.

        branchedNode->SetPrimaryMediumIndex(originatingNode->GetPrimaryMediumIndex());
        branchedNode->Properties() = originatingNode->Properties();
        branchedNode->SetReadQuorum(originatingNode->GetReadQuorum());
        branchedNode->SetWriteQuorum(originatingNode->GetWriteQuorum());

        if (!originatingNode->IsExternal()) {
            auto* chunkList = originatingNode->GetChunkList();
            branchedNode->SetChunkList(chunkList);

            chunkList->AddOwningNode(branchedNode);

            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->RefObject(chunkList);
        }
    }

    virtual void DoLogBranch(
        const TJournalNode* originatingNode,
        TJournalNode* branchedNode,
        ELockMode mode) override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto* primaryMedium = chunkManager->GetMediumByIndex(originatingNode->GetPrimaryMediumIndex());
        LOG_DEBUG_UNLESS(
            IsRecovery(),
            "Node branched (OriginatingNodeId: %v, BranchedNodeId: %v, ChunkListId: %v, "
            "PrimaryMedium: %v, Properties: %v, ReadQuorum: %v, WriteQuorum: %v, Mode: %v)",
            originatingNode->GetVersionedId(),
            branchedNode->GetVersionedId(),
            GetObjectId(originatingNode->GetChunkList()),
            primaryMedium->GetName(),
            originatingNode->Properties(),
            originatingNode->GetReadQuorum(),
            originatingNode->GetWriteQuorum(),
            mode);
    }

    virtual void DoMerge(
        TJournalNode* originatingNode,
        TJournalNode* branchedNode) override
    {
        // NB: Don't call TBase::DoMerge.

        YCHECK(originatingNode->GetChunkList() == branchedNode->GetChunkList());
        auto* chunkList = originatingNode->GetChunkList();

        if (!originatingNode->IsExternal()) {
            chunkList->RemoveOwningNode(branchedNode);

            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->UnrefObject(chunkList);
        }

        HandleTransactionFinished(originatingNode, branchedNode);
    }

    virtual void DoLogMerge(
        TJournalNode* originatingNode,
        TJournalNode* branchedNode) override
    {
        LOG_DEBUG_UNLESS(
            IsRecovery(),
            "Node merged (OriginatingNodeId: %v, BranchedNodeId: %v, ChunkListId: %v)",
            originatingNode->GetVersionedId(),
            branchedNode->GetVersionedId(),
            GetObjectId(originatingNode->GetChunkList()));
    }

    virtual void DoUnbranch(
        TJournalNode* originatingNode,
        TJournalNode* branchedNode) override
    {
        // NB: Don't call TBase::DoUnbranch.

        YCHECK(originatingNode->GetChunkList() == branchedNode->GetChunkList());

        HandleTransactionFinished(originatingNode, branchedNode);
    }

    virtual void DoLogUnbranch(
        TJournalNode* originatingNode,
        TJournalNode* branchedNode) override
    {
        LOG_DEBUG_UNLESS(
            IsRecovery(),
            "Node unbranched (OriginatingNodeId: %v, BranchedNodeId: %v, ChunkListId: %v)",
            originatingNode->GetVersionedId(),
            branchedNode->GetVersionedId(),
            GetObjectId(originatingNode->GetChunkList()));
    }

    virtual void DoClone(
        TJournalNode* sourceNode,
        TJournalNode* clonedNode,
        ICypressNodeFactory* factory,
        ENodeCloneMode mode) override
    {
        if (mode == ENodeCloneMode::Copy) {
            THROW_ERROR_EXCEPTION("Journals cannot be copied");
        }

        if (!sourceNode->GetSealed()) {
            THROW_ERROR_EXCEPTION("Journal is not sealed");
        }

        clonedNode->SetReadQuorum(sourceNode->GetReadQuorum());
        clonedNode->SetWriteQuorum(sourceNode->GetWriteQuorum());

        TBase::DoClone(sourceNode, clonedNode, factory, mode);
    }

    void HandleTransactionFinished(TJournalNode* originatingNode, TJournalNode* branchedNode)
    {
        if (branchedNode->GetUpdateMode() != EUpdateMode::Append)
            return;

        auto* trunkNode = branchedNode->GetTrunkNode();
        if (!trunkNode->IsExternal()) {
            auto* trailingChunk = trunkNode->GetTrailingChunk();
            if (trailingChunk && !trailingChunk->IsSealed()) {
                LOG_DEBUG_UNLESS(
                    IsRecovery(),
                    "Waiting for the trailing journal chunk to become sealed (NodeId: %v, ChunkId: %v)",
                    trunkNode->GetId(),
                    trailingChunk->GetId());
                const auto& chunkManager = Bootstrap_->GetChunkManager();
                chunkManager->ScheduleChunkSeal(trailingChunk);
            } else {
                const auto& journalManager = Bootstrap_->GetJournalManager();
                journalManager->SealJournal(trunkNode, nullptr);
            }
        }
    }

    virtual int GetDefaultReplicationFactor() const override
    {
        return Bootstrap_->GetConfig()->CypressManager->DefaultJournalReplicationFactor;
    }
};

INodeTypeHandlerPtr CreateJournalTypeHandler(TBootstrap* bootstrap)
{
    return New<TJournalNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJournalServer
} // namespace NYT

