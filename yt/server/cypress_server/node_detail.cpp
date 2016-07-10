#include "node_detail.h"
#include "helpers.h"
#include "node_proxy_detail.h"

#include <yt/server/cell_master/hydra_facade.h>

#include <yt/server/security_server/security_manager.h>
#include <yt/server/security_server/user.h>

namespace NYT {
namespace NCypressServer {

using namespace NYTree;
using namespace NYson;
using namespace NTransactionServer;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NSecurityServer;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

const EObjectType TCypressScalarTypeTraits<Stroka>::ObjectType = EObjectType::StringNode;
const ENodeType   TCypressScalarTypeTraits<Stroka>::NodeType   = ENodeType::String;
const EObjectType TCypressScalarTypeTraits<i64>::ObjectType    = EObjectType::Int64Node;
const ENodeType   TCypressScalarTypeTraits<i64>::NodeType      = ENodeType::Int64;
const EObjectType TCypressScalarTypeTraits<ui64>::ObjectType   = EObjectType::Uint64Node;
const ENodeType   TCypressScalarTypeTraits<ui64>::NodeType     = ENodeType::Uint64;
const EObjectType TCypressScalarTypeTraits<double>::ObjectType = EObjectType::DoubleNode;
const ENodeType   TCypressScalarTypeTraits<double>::NodeType   = ENodeType::Double;
const EObjectType TCypressScalarTypeTraits<bool>::ObjectType   = EObjectType::BooleanNode;
const ENodeType   TCypressScalarTypeTraits<bool>::NodeType     = ENodeType::Boolean;


} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

TNontemplateCypressNodeTypeHandlerBase::TNontemplateCypressNodeTypeHandlerBase(
    NCellMaster::TBootstrap* bootstrap)
    : Bootstrap_(bootstrap)
{ }

bool TNontemplateCypressNodeTypeHandlerBase::IsExternalizable() const
{
    return false;
}

EPermissionSet TNontemplateCypressNodeTypeHandlerBase::GetSupportedPermissions() const
{
    return
        EPermissionSet::Read |
        EPermissionSet::Write |
        EPermissionSet::Remove |
        EPermissionSet::Administer |
        EPermissionSet::Mount;
}

bool TNontemplateCypressNodeTypeHandlerBase::IsLeader() const
{
    return Bootstrap_->GetHydraFacade()->GetHydraManager()->IsLeader();
}

bool TNontemplateCypressNodeTypeHandlerBase::IsRecovery() const
{
    return Bootstrap_->GetHydraFacade()->GetHydraManager()->IsRecovery();
}

void TNontemplateCypressNodeTypeHandlerBase::DestroyCore(TCypressNodeBase* node)
{
    auto securityManager = Bootstrap_->GetSecurityManager();

    // Reset parent links from immediate descendants.
    for (auto* descendant : node->ImmediateDescendants()) {
        descendant->ResetParent();
    }
    node->ImmediateDescendants().clear();
    node->SetParent(nullptr);

    // Reset account.
    securityManager->ResetAccount(node);

    // Clear ACD to unregister the node from linked objects.
    node->Acd().Clear();
}

void TNontemplateCypressNodeTypeHandlerBase::BranchCore(
    TCypressNodeBase* originatingNode,
    TCypressNodeBase* branchedNode,
    TTransaction* transaction,
    ELockMode mode)
{
    auto objectManager = Bootstrap_->GetObjectManager();

    // Copy basic properties.
    branchedNode->SetParent(originatingNode->GetParent());
    branchedNode->SetCreationTime(originatingNode->GetCreationTime());
    branchedNode->SetModificationTime(originatingNode->GetModificationTime());
    branchedNode->SetRevision(originatingNode->GetRevision());
    branchedNode->SetLockMode(mode);
    branchedNode->SetTrunkNode(originatingNode->GetTrunkNode());
    branchedNode->SetTransaction(transaction);
    branchedNode->SetOriginator(originatingNode);
    branchedNode->SetExternalCellTag(originatingNode->GetExternalCellTag());

    auto securityManager = Bootstrap_->GetSecurityManager();
    securityManager->SetNodeResourceAccounting(branchedNode, originatingNode->GetAccountingEnabled());

    // Branch user attributes.
    objectManager->BranchAttributes(originatingNode, branchedNode);
}

void TNontemplateCypressNodeTypeHandlerBase::MergeCore(
    TCypressNodeBase* originatingNode,
    TCypressNodeBase* branchedNode)
{
    auto objectManager = Bootstrap_->GetObjectManager();
    auto securityManager = Bootstrap_->GetSecurityManager();

    // Merge user attributes.
    objectManager->MergeAttributes(originatingNode, branchedNode);

    // Perform cleanup by resetting the parent link of the branched node.
    branchedNode->SetParent(nullptr);

    // Reset account.
    securityManager->ResetAccount(branchedNode);

    // Merge modification time.
    const auto* mutationContext = NHydra::GetCurrentMutationContext();
    originatingNode->SetModificationTime(mutationContext->GetTimestamp());
    originatingNode->SetRevision(mutationContext->GetVersion().ToRevision());
}

TCypressNodeBase* TNontemplateCypressNodeTypeHandlerBase::CloneCorePrologue(
    ICypressNodeFactory* factory,
    const TNodeId& hintId,
    TCellTag externalCellTag)
{
    auto type = GetObjectType();
    auto objectManager = Bootstrap_->GetObjectManager();
    auto clonedId = hintId
        ? hintId
        : objectManager->GenerateId(type, NullObjectId);
    return factory->InstantiateNode(clonedId, externalCellTag);
}

void TNontemplateCypressNodeTypeHandlerBase::CloneCoreEpilogue(
    TCypressNodeBase* sourceNode,
    TCypressNodeBase* clonedNode,
    ICypressNodeFactory* factory)
{
    // Copy attributes directly to suppress validation.
    auto keyToAttribute = GetNodeAttributes(
        Bootstrap_->GetCypressManager(),
        sourceNode->GetTrunkNode(),
        factory->GetTransaction());
    if (!keyToAttribute.empty()) {
        auto* clonedAttributes = clonedNode->GetMutableAttributes();
        for (const auto& pair : keyToAttribute) {
            YCHECK(clonedAttributes->Attributes().insert(pair).second);
        }
    }

    // Copy ACD.
    clonedNode->Acd().SetInherit(sourceNode->Acd().GetInherit());
    for (const auto& ace : sourceNode->Acd().Acl().Entries) {
        clonedNode->Acd().AddEntry(ace);
    }
}

////////////////////////////////////////////////////////////////////////////////

TMapNode::TMapNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
    , ChildCountDelta_(0)
{ }

ENodeType TMapNode::GetNodeType() const
{
    return ENodeType::Map;
}

void TMapNode::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);

    using NYT::Save;
    Save(context, ChildCountDelta_);
    TMapSerializer<
        TDefaultSerializer,
        TNonversionedObjectRefSerializer
    >::Save(context, KeyToChild_);
}

void TMapNode::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);

    using NYT::Load;
    Load(context, ChildCountDelta_);
    TMapSerializer<
        TDefaultSerializer,
        TNonversionedObjectRefSerializer
    >::Load(context, KeyToChild_);

    // Reconstruct ChildToKey map.
    for (const auto& pair : KeyToChild_) {
        const auto& key = pair.first;
        auto* child = pair.second;
        if (child) {
            YCHECK(ChildToKey_.insert(std::make_pair(child, key)).second);
        }
    }
}

int TMapNode::GetGCWeight() const
{
    return TObjectBase::GetGCWeight() + KeyToChild_.size();
}

////////////////////////////////////////////////////////////////////////////////

TMapNodeTypeHandler::TMapNodeTypeHandler(TBootstrap* bootstrap)
    : TBase(bootstrap)
{ }

EObjectType TMapNodeTypeHandler::GetObjectType() const
{
    return EObjectType::MapNode;
}

ENodeType TMapNodeTypeHandler::GetNodeType() const
{
    return ENodeType::Map;
}

void TMapNodeTypeHandler::DoDestroy(TMapNode* node)
{
    TBase::DoDestroy(node);

    // Drop references to the children.
    // Make sure we handle them in a stable order.
    auto objectManager = Bootstrap_->GetObjectManager();
    for (const auto& pair : SortKeyToChild(node->KeyToChild())) {
        auto* node = pair.second;
        if (node) {
            objectManager->UnrefObject(node);
        }
    }
}

void TMapNodeTypeHandler::DoBranch(
    const TMapNode* originatingNode,
    TMapNode* branchedNode,
    ELockMode mode)
{
    TBase::DoBranch(originatingNode, branchedNode, mode);
}

void TMapNodeTypeHandler::DoMerge(
    TMapNode* originatingNode,
    TMapNode* branchedNode)
{
    TBase::DoMerge(originatingNode, branchedNode);

    auto objectManager = Bootstrap_->GetObjectManager();

    bool isOriginatingNodeBranched = originatingNode->GetTransaction() != nullptr;

    auto& keyToChild = originatingNode->KeyToChild();
    auto& childToKey = originatingNode->ChildToKey();

    for (const auto& pair : SortKeyToChild(branchedNode->KeyToChild())) {
        const auto& key = pair.first;
        auto* childTrunkNode = pair.second;

        auto it = keyToChild.find(key);
        if (childTrunkNode) {
            if (it == keyToChild.end()) {
                // Originating: missing
                YCHECK(childToKey.insert(std::make_pair(childTrunkNode, key)).second);
                YCHECK(keyToChild.insert(std::make_pair(key, childTrunkNode)).second);
            } else if (it->second) {
                // Originating: present
                objectManager->UnrefObject(it->second);
                YCHECK(childToKey.erase(it->second) == 1);
                YCHECK(childToKey.insert(std::make_pair(childTrunkNode, key)).second);
                it->second = childTrunkNode;;
            } else {
                // Originating: tombstone
                it->second = childTrunkNode;
                YCHECK(childToKey.insert(std::make_pair(childTrunkNode, key)).second);
            }
        } else {
            // Branched: tombstone
            if (it == keyToChild.end()) {
                // Originating: missing
                if (isOriginatingNodeBranched) {
                    YCHECK(keyToChild.insert(std::make_pair(key, nullptr)).second);
                }
            } else if (it->second) {
                // Originating: present
                objectManager->UnrefObject(it->second);
                YCHECK(childToKey.erase(it->second) == 1);
                if (isOriginatingNodeBranched) {
                    it->second = nullptr;
                } else {
                    keyToChild.erase(it);
                }
            } else {
                // Originating: tombstone
            }
        }
    }

    originatingNode->ChildCountDelta() += branchedNode->ChildCountDelta();
}

ICypressNodeProxyPtr TMapNodeTypeHandler::DoGetProxy(
    TMapNode* trunkNode,
    TTransaction* transaction)
{
    return New<TMapNodeProxy>(
        Bootstrap_,
        &Metadata_,
        transaction,
        trunkNode);
}

void TMapNodeTypeHandler::DoClone(
    TMapNode* sourceNode,
    TMapNode* clonedNode,
    ICypressNodeFactory* factory,
    ENodeCloneMode mode)
{
    TBase::DoClone(sourceNode, clonedNode, factory, mode);

    auto* transaction = factory->GetTransaction();

    auto cypressManager = Bootstrap_->GetCypressManager();

    auto keyToChildMap = GetMapNodeChildMap(
        cypressManager,
        sourceNode->GetTrunkNode(),
        transaction);
    auto keyToChildList = SortKeyToChild(keyToChildMap);

    auto* clonedTrunkNode = clonedNode->GetTrunkNode();

    auto objectManager = Bootstrap_->GetObjectManager();

    for (const auto& pair : keyToChildList) {
        const auto& key = pair.first;
        auto* childTrunkNode = pair.second;

        auto* childNode = cypressManager->GetVersionedNode(childTrunkNode, transaction);

        auto* clonedChildNode = factory->CloneNode(childNode, mode);
        auto* clonedTrunkChildNode = clonedChildNode->GetTrunkNode();

        YCHECK(clonedNode->KeyToChild().insert(std::make_pair(key, clonedTrunkChildNode)).second);
        YCHECK(clonedNode->ChildToKey().insert(std::make_pair(clonedTrunkChildNode, key)).second);

        AttachChild(objectManager, clonedTrunkNode, clonedChildNode);

        ++clonedNode->ChildCountDelta();
    }
}

////////////////////////////////////////////////////////////////////////////////

TListNode::TListNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
{ }

ENodeType TListNode::GetNodeType() const
{
    return ENodeType::List;
}

void TListNode::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);

    using NYT::Save;
    TVectorSerializer<
        TNonversionedObjectRefSerializer
    >::Save(context, IndexToChild_);
}

void TListNode::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);

    using NYT::Load;
    TVectorSerializer<
        TNonversionedObjectRefSerializer
    >::Load(context, IndexToChild_);

    // Reconstruct ChildToIndex.
    for (int index = 0; index < IndexToChild_.size(); ++index) {
        YCHECK(ChildToIndex_.insert(std::make_pair(IndexToChild_[index], index)).second);
    }
}

int TListNode::GetGCWeight() const
{
    return TObjectBase::GetGCWeight() + IndexToChild_.size();
}

////////////////////////////////////////////////////////////////////////////////

TListNodeTypeHandler::TListNodeTypeHandler(TBootstrap* bootstrap)
    : TBase(bootstrap)
{ }

EObjectType TListNodeTypeHandler::GetObjectType() const
{
    return EObjectType::ListNode;
}

ENodeType TListNodeTypeHandler::GetNodeType() const
{
    return ENodeType::List;
}

ICypressNodeProxyPtr TListNodeTypeHandler::DoGetProxy(
    TListNode* trunkNode,
    TTransaction* transaction)
{
    return New<TListNodeProxy>(
        Bootstrap_,
        &Metadata_,
        transaction,
        trunkNode);
}

void TListNodeTypeHandler::DoDestroy(TListNode* node)
{
    TBase::DoDestroy(node);

    // Drop references to the children.
    auto objectManager = Bootstrap_->GetObjectManager();
    for (auto* child : node->IndexToChild()) {
        objectManager->UnrefObject(child);
    }
}

void TListNodeTypeHandler::DoBranch(
    const TListNode* originatingNode,
    TListNode* branchedNode,
    ELockMode mode)
{
    TBase::DoBranch(originatingNode, branchedNode, mode);

    branchedNode->IndexToChild() = originatingNode->IndexToChild();
    branchedNode->ChildToIndex() = originatingNode->ChildToIndex();

    // Reference all children.
    auto objectManager = Bootstrap_->GetObjectManager();
    for (auto* child : originatingNode->IndexToChild()) {
        objectManager->RefObject(child);
    }
}

void TListNodeTypeHandler::DoMerge(
    TListNode* originatingNode,
    TListNode* branchedNode)
{
    TBase::DoMerge(originatingNode, branchedNode);

    // Drop all references held by the originator.
    auto objectManager = Bootstrap_->GetObjectManager();
    for (auto* child : originatingNode->IndexToChild()) {
        objectManager->UnrefObject(child);
    }

    // Replace the child list with the branched copy.
    originatingNode->IndexToChild().swap(branchedNode->IndexToChild());
    originatingNode->ChildToIndex().swap(branchedNode->ChildToIndex());
}

void TListNodeTypeHandler::DoClone(
    TListNode* sourceNode,
    TListNode* clonedNode,
    ICypressNodeFactory* factory,
    ENodeCloneMode mode)
{
    TBase::DoClone(sourceNode, clonedNode, factory, mode);

    auto* clonedTrunkNode = clonedNode->GetTrunkNode();

    auto objectManager = Bootstrap_->GetObjectManager();
    const auto& indexToChild = sourceNode->IndexToChild();
    for (int index = 0; index < indexToChild.size(); ++index) {
        auto* childNode = indexToChild[index];
        auto* clonedChildNode = factory->CloneNode(childNode, mode);
        auto* clonedChildTrunkNode = clonedChildNode->GetTrunkNode();

        clonedNode->IndexToChild().push_back(clonedChildTrunkNode);
        YCHECK(clonedNode->ChildToIndex().insert(std::make_pair(clonedChildTrunkNode, index)).second);

        AttachChild(objectManager, clonedTrunkNode, clonedChildNode);
    }
}

////////////////////////////////////////////////////////////////////////////////

TLinkNode::TLinkNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
{ }

ENodeType TLinkNode::GetNodeType() const
{
    return ENodeType::Entity;
}

void TLinkNode::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);
    
    using NYT::Save;
    Save(context, TargetId_);
}

void TLinkNode::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);
    
    using NYT::Load;
    Load(context, TargetId_);
}

////////////////////////////////////////////////////////////////////////////////

TLinkNodeTypeHandler::TLinkNodeTypeHandler(NCellMaster::TBootstrap* bootstrap)
    : TBase(bootstrap)
{ }

EObjectType TLinkNodeTypeHandler::GetObjectType() const
{
    return EObjectType::Link;
}

ENodeType TLinkNodeTypeHandler::GetNodeType() const
{
    return ENodeType::Entity;
}

ICypressNodeProxyPtr TLinkNodeTypeHandler::DoGetProxy(
    TLinkNode* trunkNode,
    TTransaction* transaction)
{
    return New<TLinkNodeProxy>(
        Bootstrap_,
        &Metadata_,
        transaction,
        trunkNode);
}

std::unique_ptr<TLinkNode> TLinkNodeTypeHandler::DoCreate(
    const TVersionedNodeId& id,
    TCellTag cellTag,
    TTransaction* transaction,
    IAttributeDictionary* attributes)
{
    // Resolve target_path using the appropriate transaction.
    auto targetPath = attributes->Find<Stroka>("target_path");
    if (targetPath) {
        attributes->Remove("target_path");

        auto objectManager = Bootstrap_->GetObjectManager();
        auto* resolver = objectManager->GetObjectResolver();

        auto targetProxy = resolver->ResolvePath(*targetPath, transaction);
        attributes->Set("target_id", targetProxy->GetId());
    }

    return TBase::DoCreate(
        id,
        cellTag,
        transaction,
        attributes);
}

void TLinkNodeTypeHandler::DoBranch(
    const TLinkNode* originatingNode,
    TLinkNode* branchedNode,
    ELockMode mode)
{
    TBase::DoBranch(originatingNode, branchedNode, mode);

    branchedNode->SetTargetId(originatingNode->GetTargetId());
}

void TLinkNodeTypeHandler::DoMerge(
    TLinkNode* originatingNode,
    TLinkNode* branchedNode)
{
    TBase::DoMerge(originatingNode, branchedNode);

    originatingNode->SetTargetId(branchedNode->GetTargetId());
}

void TLinkNodeTypeHandler::DoClone(
    TLinkNode* sourceNode,
    TLinkNode* clonedNode,
    ICypressNodeFactory* factory,
    ENodeCloneMode mode)
{
    TBase::DoClone(sourceNode, clonedNode, factory, mode);

    clonedNode->SetTargetId(sourceNode->GetTargetId());
}

////////////////////////////////////////////////////////////////////////////////

TDocumentNode::TDocumentNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
    , Value_(GetEphemeralNodeFactory()->CreateEntity())
{ }

ENodeType TDocumentNode::GetNodeType() const
{
    return ENodeType::Entity;
}

void TDocumentNode::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);

    using NYT::Save;
    auto serializedValue = ConvertToYsonStringStable(Value_);
    Save(context, serializedValue.Data());
}

void TDocumentNode::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);

    using NYT::Load;
    auto serializedValue = Load<Stroka>(context);
    Value_ = ConvertToNode(TYsonString(serializedValue));
}

////////////////////////////////////////////////////////////////////////////////

TDocumentNodeTypeHandler::TDocumentNodeTypeHandler(NCellMaster::TBootstrap* bootstrap)
    : TBase(bootstrap)
{ }

EObjectType TDocumentNodeTypeHandler::GetObjectType() const
{
    return EObjectType::Document;
}

ENodeType TDocumentNodeTypeHandler::GetNodeType() const
{
    return ENodeType::Entity;
}

ICypressNodeProxyPtr TDocumentNodeTypeHandler::DoGetProxy(
    TDocumentNode* trunkNode,
    TTransaction* transaction)
{
    return New<TDocumentNodeProxy>(
        Bootstrap_,
        &Metadata_,
        transaction,
        trunkNode);
}

void TDocumentNodeTypeHandler::DoBranch(
    const TDocumentNode* originatingNode,
    TDocumentNode* branchedNode,
    ELockMode mode)
{
    TBase::DoBranch(originatingNode, branchedNode, mode);

    branchedNode->SetValue(CloneNode(originatingNode->GetValue()));
}

void TDocumentNodeTypeHandler::DoMerge(
    TDocumentNode* originatingNode,
    TDocumentNode* branchedNode)
{
    TBase::DoMerge(originatingNode, branchedNode);

    originatingNode->SetValue(branchedNode->GetValue());
}

void TDocumentNodeTypeHandler::DoClone(
    TDocumentNode* sourceNode,
    TDocumentNode* clonedNode,
    ICypressNodeFactory* factory,
    ENodeCloneMode mode)
{
    TBase::DoClone(sourceNode, clonedNode, factory, mode);

    clonedNode->SetValue(CloneNode(sourceNode->GetValue()));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

