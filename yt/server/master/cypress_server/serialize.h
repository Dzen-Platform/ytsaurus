#pragma once

#include "public.h"

#include <yt/server/master/transaction_server/public.h>

#include <yt/server/master/object_server/public.h>

#include <yt/server/master/chunk_server/public.h>

#include <yt/server/master/tablet_server/public.h>

#include <yt/server/master/security_server/public.h>

#include <yt/server/master/transaction_server/public.h>

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/table_server/public.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TBeginCopyContext
    : public TEntityStreamSaveContext
{
public:
    TBeginCopyContext(
        NTransactionServer::TTransaction* transaction,
        ENodeCloneMode Mode);

    void RegisterOpaqueRootId(TNodeId rootId);
    void RegisterExternalCellTag(NObjectClient::TCellTag cellTag);

    DEFINE_BYREF_RO_PROPERTY(std::vector<TNodeId>, OpaqueRootIds);
    DEFINE_BYVAL_RO_PROPERTY(NTransactionServer::TTransaction*, Transaction);
    DEFINE_BYVAL_RO_PROPERTY(ENodeCloneMode, Mode);

    TString Finish();
    NObjectClient::TCellTagList GetExternalCellTags();

    // TODO(babenko): get rid of this separate registry
    const NTableServer::TTableSchemaRegistryPtr& GetTableSchemaRegistry() const;

private:
    // TODO(babenko): get rid of this separate registry
    const NTableServer::TTableSchemaRegistryPtr TableSchemaRegistry_;
    TString Data_;
    TStringOutput Stream_;
    std::vector<NObjectClient::TCellTag> ExternalCellTags_;
};

////////////////////////////////////////////////////////////////////////////////

class TEndCopyContext
    : public TEntityStreamLoadContext
{
public:
    TEndCopyContext(
        NCellMaster::TBootstrap* bootstrap,
        ENodeCloneMode mode,
        TRef data);

    template <class T>
    T* GetObject(NObjectServer::TObjectId id);

    template <class T>
    const TInternRegistryPtr<T>& GetInternRegistry() const;

    DEFINE_BYVAL_RO_PROPERTY(ENodeCloneMode, Mode);

private:
    NCellMaster::TBootstrap* const Bootstrap_;
    // TODO(babenko): get rid of this separate registry
    const NTableServer::TTableSchemaRegistryPtr TableSchemaRegistry_;

    TMemoryInput Stream_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
