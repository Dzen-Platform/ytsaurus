#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/server/object_server/public.h>

#include <yt/core/yson/string.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

const yhash<TString, TCypressNodeBase*>& GetMapNodeChildMap(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction,
    yhash<TString, TCypressNodeBase*>* storage);

std::vector<TCypressNodeBase*> GetMapNodeChildList(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

const std::vector<TCypressNodeBase*>& GetListNodeChildList(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

std::vector<std::pair<TString, TCypressNodeBase*>> SortKeyToChild(
    const yhash<TString, TCypressNodeBase*>& keyToChildMap);

TCypressNodeBase* FindMapNodeChild(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction,
    const TString& key);

yhash<TString, NYson::TYsonString> GetNodeAttributes(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

yhash_set<TString> ListNodeAttributes(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

void AttachChild(
    const NObjectServer::TObjectManagerPtr& objectManager,
    TCypressNodeBase* trunkParent,
    TCypressNodeBase* child);

void DetachChild(
    const NObjectServer::TObjectManagerPtr& objectManager,
    TCypressNodeBase* trunkParent,
    TCypressNodeBase* child,
    bool unref);

bool NodeHasKey(const TCypressNodeBase* node);

bool IsParentOf(
    const TCypressNodeBase* parent,
    const TCypressNodeBase* descendant);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
