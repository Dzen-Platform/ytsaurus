#pragma once

#include "public.h"
#include "rpc_proxy_connection.h"
#include "api_service_proxy.h"

#include <yt/ytlib/api/client.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

class TRpcProxyClientBase
    : public virtual NApi::IClientBase
{
protected:
    // Must return a bound RPC proxy connection for this interface.
    virtual TRpcProxyConnectionPtr GetRpcProxyConnection() = 0;
    // Must return an RPC channel to use for API call.
    virtual NRpc::IChannelPtr GetChannel() = 0;

public:
    TRpcProxyClientBase();
    ~TRpcProxyClientBase();

    virtual NApi::IConnectionPtr GetConnection() override
    {
        return GetRpcProxyConnection();
    }

    // Transactions
    virtual TFuture<NApi::ITransactionPtr> StartTransaction(
        NTransactionClient::ETransactionType type,
        const NApi::TTransactionStartOptions& options) override;

    // Tables
    virtual TFuture<NApi::IUnversionedRowsetPtr> LookupRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const NApi::TLookupRowsOptions& options) override;

    virtual TFuture<NApi::IVersionedRowsetPtr> VersionedLookupRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const NApi::TVersionedLookupRowsOptions& options) override;

    virtual TFuture<NApi::TSelectRowsResult> SelectRows(
        const TString& query,
        const NApi::TSelectRowsOptions& options) override;

    // TODO(babenko): batch read and batch write

    // Cypress
    virtual TFuture<NYson::TYsonString> GetNode(
        const NYPath::TYPath& path,
        const NApi::TGetNodeOptions& options) override;

    virtual TFuture<void> SetNode(
        const NYPath::TYPath& path,
        const NYson::TYsonString& value,
        const NApi::TSetNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<void> RemoveNode(
        const NYPath::TYPath& path,
        const NApi::TRemoveNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NYson::TYsonString> ListNode(
        const NYPath::TYPath& path,
        const NApi::TListNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NCypressClient::TNodeId> CreateNode(
        const NYPath::TYPath& path,
        NObjectClient::EObjectType type,
        const NApi::TCreateNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NApi::TLockNodeResult> LockNode(
        const NYPath::TYPath& path,
        NCypressClient::ELockMode mode,
        const NApi::TLockNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NCypressClient::TNodeId> CopyNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TCopyNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NCypressClient::TNodeId> MoveNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TMoveNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<NCypressClient::TNodeId> LinkNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TLinkNodeOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual TFuture<void> ConcatenateNodes(
        const std::vector<NYPath::TYPath>& srcPaths,
        const NYPath::TYPath& dstPath,
        const NApi::TConcatenateNodesOptions& options) override
    {
        Y_UNIMPLEMENTED();
    };

    virtual TFuture<bool> NodeExists(
        const NYPath::TYPath& path,
        const NApi::TNodeExistsOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }


    // Objects
    virtual TFuture<NObjectClient::TObjectId> CreateObject(
        NObjectClient::EObjectType type,
        const NApi::TCreateObjectOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }


    // Files
    virtual TFuture<NConcurrency::IAsyncZeroCopyInputStreamPtr> CreateFileReader(
        const NYPath::TYPath& path,
        const NApi::TFileReaderOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual NApi::IFileWriterPtr CreateFileWriter(
        const NYPath::TYPath& path,
        const NApi::TFileWriterOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }


    // Journals
    virtual NApi::IJournalReaderPtr CreateJournalReader(
        const NYPath::TYPath& path,
        const NApi::TJournalReaderOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }

    virtual NApi::IJournalWriterPtr CreateJournalWriter(
        const NYPath::TYPath& path,
        const NApi::TJournalWriterOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }


    // Tables
    virtual TFuture<NTableClient::ISchemalessMultiChunkReaderPtr> CreateTableReader(
        const NYPath::TRichYPath& path,
        const NApi::TTableReaderOptions& options) override
    {
        Y_UNIMPLEMENTED();
    }
};

DEFINE_REFCOUNTED_TYPE(TRpcProxyClientBase)

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NYT
