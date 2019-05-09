#pragma once

#include "client_base.h"

#include <yt/client/api/transaction.h>

#include <yt/core/rpc/public.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETransactionState,
    (Active)
    (Aborted)
    (Committing)
    (Committed)
    (Detached)
);

class TTransaction
    : public NApi::ITransaction
{
public:
    TTransaction(
        TConnectionPtr connection,
        TClientPtr client,
        NRpc::IChannelPtr channel,
        NTransactionClient::TTransactionId id,
        NTransactionClient::TTimestamp startTimestamp,
        NTransactionClient::ETransactionType type,
        NTransactionClient::EAtomicity atomicity,
        NTransactionClient::EDurability durability,
        TDuration timeout,
        std::optional<TDuration> pingPeriod,
        bool sticky);

    // ITransaction implementation
    virtual NApi::IConnectionPtr GetConnection() override;
    virtual NApi::IClientPtr GetClient() const override;

    virtual NTransactionClient::ETransactionType GetType() const override;
    virtual NTransactionClient::TTransactionId GetId() const override;
    virtual NTransactionClient::TTimestamp GetStartTimestamp() const override;
    virtual NTransactionClient::EAtomicity GetAtomicity() const override;
    virtual NTransactionClient::EDurability GetDurability() const override;
    virtual TDuration GetTimeout() const override;

    virtual TFuture<void> Ping(const NApi::TTransactionPingOptions& options = {}) override;
    virtual TFuture<NApi::TTransactionCommitResult> Commit(const NApi::TTransactionCommitOptions&) override;
    virtual TFuture<void> Abort(const NApi::TTransactionAbortOptions& options) override;
    virtual void Detach() override;
    virtual TFuture<NApi::TTransactionPrepareResult> Prepare() override;
    virtual TFuture<NApi::TTransactionFlushResult> Flush() override;

    virtual void SubscribeCommitted(const TClosure&) override;
    virtual void UnsubscribeCommitted(const TClosure&) override;

    virtual void SubscribeAborted(const TClosure&) override;
    virtual void UnsubscribeAborted(const TClosure&) override;

    virtual void ModifyRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        TSharedRange<NApi::TRowModification> modifications,
        const NApi::TModifyRowsOptions& options) override;

    // IClientBase implementation
    virtual TFuture<NApi::ITransactionPtr> StartTransaction(
        NTransactionClient::ETransactionType type,
        const NApi::TTransactionStartOptions& options) override;

    virtual TFuture<NApi::IUnversionedRowsetPtr> LookupRows(
        const NYPath::TYPath& path, NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const NApi::TLookupRowsOptions& options) override;

    virtual TFuture<NApi::IVersionedRowsetPtr> VersionedLookupRows(
        const NYPath::TYPath& path, NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const NApi::TVersionedLookupRowsOptions& options) override;

    virtual TFuture<NApi::TSelectRowsResult> SelectRows(
        const TString& query,
        const NApi::TSelectRowsOptions& options) override;

    virtual TFuture<ITableReaderPtr> CreateTableReader(
        const NYPath::TRichYPath& path,
        const NApi::TTableReaderOptions& options) override;

    virtual TFuture<ITableWriterPtr> CreateTableWriter(
        const NYPath::TRichYPath& path,
        const NApi::TTableWriterOptions& options) override;

    virtual TFuture<NYson::TYsonString> GetNode(
        const NYPath::TYPath& path,
        const NApi::TGetNodeOptions& options) override;

    virtual TFuture<void> SetNode(
        const NYPath::TYPath& path,
        const NYson::TYsonString& value,
        const NApi::TSetNodeOptions& options) override;

    virtual TFuture<void> RemoveNode(
        const NYPath::TYPath& path,
        const NApi::TRemoveNodeOptions& options) override;

    virtual TFuture<NYson::TYsonString> ListNode(
        const NYPath::TYPath& path,
        const NApi::TListNodeOptions& options) override;

    virtual TFuture<NCypressClient::TNodeId> CreateNode(
        const NYPath::TYPath& path,
        NObjectClient::EObjectType type,
        const NApi::TCreateNodeOptions& options) override;

    virtual TFuture<NApi::TLockNodeResult> LockNode(
        const NYPath::TYPath& path,
        NCypressClient::ELockMode mode,
        const NApi::TLockNodeOptions& options) override;

    virtual TFuture<void> UnlockNode(
        const NYPath::TYPath& path,
        const NApi::TUnlockNodeOptions& options) override;

    virtual TFuture<NCypressClient::TNodeId> CopyNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TCopyNodeOptions& options) override;

    virtual TFuture<NCypressClient::TNodeId> MoveNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TMoveNodeOptions& options) override;

    virtual TFuture<NCypressClient::TNodeId> LinkNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const NApi::TLinkNodeOptions& options) override;

    virtual TFuture<void> ConcatenateNodes(
        const std::vector<NYPath::TRichYPath>& srcPaths,
        const NYPath::TRichYPath& dstPath,
        const NApi::TConcatenateNodesOptions& options) override;

    virtual TFuture<bool> NodeExists(
        const NYPath::TYPath& path,
        const NApi::TNodeExistsOptions& options) override;

    virtual TFuture<NObjectClient::TObjectId> CreateObject(
        NObjectClient::EObjectType type,
        const NApi::TCreateObjectOptions& options) override;

    virtual TFuture<NApi::IFileReaderPtr> CreateFileReader(
        const NYPath::TYPath& path,
        const NApi::TFileReaderOptions& options) override;

    virtual NApi::IFileWriterPtr CreateFileWriter(
        const NYPath::TRichYPath& path,
        const NApi::TFileWriterOptions& options) override;

    virtual NApi::IJournalReaderPtr CreateJournalReader(
        const NYPath::TYPath& path,
        const NApi::TJournalReaderOptions& options) override;

    virtual NApi::IJournalWriterPtr CreateJournalWriter(
        const NYPath::TYPath& path,
        const NApi::TJournalWriterOptions& options) override;

private:
    const TConnectionPtr Connection_;
    const TClientPtr Client_;
    const NRpc::IChannelPtr Channel_;
    const NTransactionClient::TTransactionId Id_;
    const NTransactionClient::TTimestamp StartTimestamp_;
    const NTransactionClient::ETransactionType Type_;
    const NTransactionClient::EAtomicity Atomicity_;
    const NTransactionClient::EDurability Durability_;
    const TDuration Timeout_;
    const std::optional<TDuration> PingPeriod_;
    const bool Sticky_;

    std::atomic<i64> ModifyRowsRequestSequenceCounter_;
    std::vector<TFuture<void>> AsyncResults_;
    TApiServiceProxy::TReqBatchModifyRowsPtr BatchModifyRowsRequest_;

    TSpinLock SpinLock_;
    TSpinLock BatchModifyRowsRequestLock_;
    TError Error_;
    ETransactionState State_ = ETransactionState::Active;

    TSingleShotCallbackList<void()> Committed_;
    TSingleShotCallbackList<void()> Aborted_;

    ETransactionState GetState();

    TApiServiceProxy CreateApiServiceProxy();

    TFuture<void> SendPing();
    void RunPeriodicPings();
    bool IsPingableState();

    void FireCommitted();
    void FireAborted();

    TError SetCommitted(const NApi::TTransactionCommitResult& result);
    // Returns true if the transaction has been aborted as a result of this call, false otherwise.
    bool SetAborted(const TError& error);
    void OnFailure(const TError& error);

    TFuture<void> SendAbort();

    void ValidateActive();
    void ValidateActive(TGuard<TSpinLock>&);

    //! Returns a fresh batch modify rows request.
    TApiServiceProxy::TReqBatchModifyRowsPtr CreateBatchModifyRowsRequest();
    //! Invokes the stored batch modify rows request and replaces it with a null one.
    TFuture<void> InvokeBatchModifyRowsRequest();

    template <class T>
    T PatchTransactionId(const T& options);
    NApi::TTransactionStartOptions PatchTransactionId(
        const NApi::TTransactionStartOptions& options);
    template <class T>
    T PatchTransactionTimestamp(const T& options);
};

DEFINE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy

#define TRANSACTION_IMPL_INL_H_
#include "transaction_impl-inl.h"
#undef TRANSACTION_IMPL_INL_H_

