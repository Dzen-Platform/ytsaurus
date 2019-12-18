#include "transaction.h"
#include "connection.h"
#include "config.h"

#include <yt/client/transaction_client/timestamp_provider.h>

#include <yt/client/object_client/helpers.h>

#include <yt/client/tablet_client/table_mount_cache.h>

#include <yt/client/table_client/proto/wire_protocol.pb.h>

#include <yt/client/table_client/wire_protocol.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/row_buffer.h>

#include <yt/client/transaction_client/helpers.h>

#include <yt/ytlib/table_client/helpers.h>

#include <yt/ytlib/api/native/tablet_helpers.h>

#include <yt/ytlib/transaction_client/transaction_manager.h>
#include <yt/ytlib/transaction_client/action.h>
#include <yt/ytlib/transaction_client/transaction_service_proxy.h>

#include <yt/ytlib/tablet_client/tablet_service_proxy.h>

#include <yt/ytlib/table_client/row_merger.h>

#include <yt/ytlib/hive/cluster_directory.h>
#include <yt/ytlib/hive/cluster_directory_synchronizer.h>

#include <yt/ytlib/query_client/column_evaluator.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/compression/codec.h>

#include <yt/core/misc/sliding_window.h>

namespace NYT::NApi::NNative {

using namespace NYPath;
using namespace NTransactionClient;
using namespace NHiveClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTabletClient;
using namespace NTableClient;
using namespace NQueryClient;
using namespace NYson;
using namespace NConcurrency;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETransactionState,
    (Active)
    (Commit)
    (Abort)
    (Prepare)
    (Flush)
    (Detach)
);

DECLARE_REFCOUNTED_CLASS(TTransaction)

class TTransaction
    : public ITransaction
{
public:
    TTransaction(
        IClientPtr client,
        NTransactionClient::TTransactionPtr transaction,
        NLogging::TLogger logger)
        : Client_(std::move(client))
        , Transaction_(std::move(transaction))
        , Logger(logger.AddTag("TransactionId: %v, ConnectionCellTag: %v",
            GetId(),
            Client_->GetConnection()->GetCellTag()))
        , OrderedRequestsSlidingWindow_(
            Client_->GetNativeConnection()->GetConfig()->MaxRequestWindowSize)
    { }


    virtual NApi::IConnectionPtr GetConnection() override
    {
        return Client_->GetConnection();
    }

    virtual NApi::IClientPtr GetClient() const override
    {
        return Client_;
    }

    virtual NTransactionClient::ETransactionType GetType() const override
    {
        return Transaction_->GetType();
    }

    virtual TTransactionId GetId() const override
    {
        return Transaction_->GetId();
    }

    virtual TTimestamp GetStartTimestamp() const override
    {
        return Transaction_->GetStartTimestamp();
    }

    virtual EAtomicity GetAtomicity() const override
    {
        return Transaction_->GetAtomicity();
    }

    virtual EDurability GetDurability() const override
    {
        return Transaction_->GetDurability();
    }

    virtual TDuration GetTimeout() const override
    {
        return Transaction_->GetTimeout();
    }


    virtual TFuture<void> Ping(const TTransactionPingOptions& options = {}) override
    {
        return Transaction_->Ping(options);
    }

    virtual TFuture<TTransactionCommitResult> Commit(const TTransactionCommitOptions& options) override
    {
        auto guard = Guard(SpinLock_);

        if (State_ != ETransactionState::Active) {
            return MakeFuture<TTransactionCommitResult>(TError("Cannot commit since transaction %v is already in %Qlv state",
                GetId(),
                State_));
        }

        State_ = ETransactionState::Commit;
        return BIND(&TTransaction::DoCommit, MakeStrong(this))
            .AsyncVia(GetThreadPoolInvoker())
            .Run(options);
    }

    virtual TFuture<void> Abort(const TTransactionAbortOptions& options) override
    {
        auto guard = Guard(SpinLock_);

        if (State_ == ETransactionState::Abort) {
            return AbortResult_;
        }

        if (State_ != ETransactionState::Active && State_ != ETransactionState::Flush && State_ != ETransactionState::Prepare) {
            return MakeFuture<void>(TError("Cannot abort since transaction %v is already in %Qlv state",
                GetId(),
                State_));
        }

        State_ = ETransactionState::Abort;
        AbortResult_ = Transaction_->Abort(options);
        return AbortResult_;
    }

    virtual void Detach() override
    {
        auto guard = Guard(SpinLock_);
        State_ = ETransactionState::Detach;
        Transaction_->Detach();
    }

    virtual TFuture<TTransactionPrepareResult> Prepare() override
    {
        auto guard = Guard(SpinLock_);

        if (State_ != ETransactionState::Active) {
            return MakeFuture<TTransactionPrepareResult>(TError("Cannot prepare since transaction %v is already in %Qlv state",
                GetId(),
                State_));
        }

        YT_LOG_DEBUG("Preparing transaction");
        State_ = ETransactionState::Prepare;
        return BIND(&TTransaction::DoPrepare, MakeStrong(this))
            .AsyncVia(GetThreadPoolInvoker())
            .Run();
    }

    virtual TFuture<TTransactionFlushResult> Flush() override
    {
        auto guard = Guard(SpinLock_);

        if (State_ != ETransactionState::Prepare) {
            return MakeFuture<TTransactionFlushResult>(TError("Cannot flush since transaction %v is already in %Qlv state",
                GetId(),
                State_));
        }

        YT_LOG_DEBUG("Flushing transaction");
        State_ = ETransactionState::Flush;
        return BIND(&TTransaction::DoFlush, MakeStrong(this))
            .AsyncVia(GetThreadPoolInvoker())
            .Run();
    }

    virtual void AddAction(TCellId cellId, const TTransactionActionData& data) override
    {
        auto guard = Guard(SpinLock_);

        YT_VERIFY(TypeFromId(cellId) == EObjectType::TabletCell ||
               TypeFromId(cellId) == EObjectType::ClusterCell);

        if (State_ != ETransactionState::Active) {
            THROW_ERROR_EXCEPTION("Cannot add action since transaction %v is already in %Qlv state",
                GetId(),
                State_);
        }

        if (GetAtomicity() != EAtomicity::Full) {
            THROW_ERROR_EXCEPTION("Atomicity must be %Qlv for custom actions",
                EAtomicity::Full);
        }

        auto session = GetOrCreateCellCommitSession(cellId);
        session->RegisterAction(data);

        YT_LOG_DEBUG("Transaction action added (CellId: %v, ActionType: %v)",
            cellId,
            data.Type);
    }


    virtual TFuture<NApi::ITransactionPtr> StartForeignTransaction(
        const NApi::IClientPtr& client,
        const TForeignTransactionStartOptions& options) override
    {
        if (client->GetConnection()->GetCellTag() == GetConnection()->GetCellTag()) {
            return MakeFuture<NApi::ITransactionPtr>(this);
        }

        TTransactionStartOptions adjustedOptions(options);
        adjustedOptions.Id = GetId();
        if (options.InheritStartTimestamp) {
            adjustedOptions.StartTimestamp = GetStartTimestamp();
        }

        return client->StartTransaction(GetType(), adjustedOptions)
            .Apply(BIND([this, this_ = MakeStrong(this)] (const NApi::ITransactionPtr& transaction) {
                RegisterForeignTransaction(transaction);
                return transaction;
            }));
    }


    virtual void SubscribeCommitted(const TClosure& callback) override
    {
        Transaction_->SubscribeCommitted(callback);
    }

    virtual void UnsubscribeCommitted(const TClosure& callback) override
    {
        Transaction_->UnsubscribeCommitted(callback);
    }


    virtual void SubscribeAborted(const TClosure& callback) override
    {
        Transaction_->SubscribeAborted(callback);
    }

    virtual void UnsubscribeAborted(const TClosure& callback) override
    {
        Transaction_->UnsubscribeAborted(callback);
    }


    virtual TFuture<ITransactionPtr> StartNativeTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        auto adjustedOptions = options;
        adjustedOptions.ParentId = GetId();
        return Client_->StartNativeTransaction(
            type,
            adjustedOptions);
    }

    virtual TFuture<NApi::ITransactionPtr> StartTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        return StartNativeTransaction(type, options).As<NApi::ITransactionPtr>();
    }

    virtual void ModifyRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        TSharedRange<TRowModification> modifications,
        const TModifyRowsOptions& options) override
    {
        auto guard = Guard(SpinLock_);

        ValidateTabletTransactionId(GetId());

        if (State_ != ETransactionState::Active) {
            THROW_ERROR_EXCEPTION("Cannot modify rows since transaction %v is already in %Qlv state",
                GetId(),
                State_);
        }

        YT_LOG_DEBUG("Buffering client row modifications (Count: %v)",
            modifications.Size());

        EnqueueModificationRequest(
            std::make_unique<TModificationRequest>(
                this,
                Client_->GetNativeConnection(),
                path,
                std::move(nameTable),
                std::move(modifications),
                options));
    }


#define DELEGATE_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        return Client_->method args; \
    }

#define DELEGATE_TRANSACTIONAL_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.TransactionId = GetId(); \
            return Client_->method args; \
        } \
    }

#define DELEGATE_TIMESTAMPED_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.Timestamp = GetReadTimestamp(); \
            return Client_->method args; \
        } \
    }

    DELEGATE_TIMESTAMPED_METHOD(TFuture<IUnversionedRowsetPtr>, LookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TLookupRowsOptions& options),
        (path, nameTable, keys, options))
    DELEGATE_TIMESTAMPED_METHOD(TFuture<IVersionedRowsetPtr>, VersionedLookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TVersionedLookupRowsOptions& options),
        (path, nameTable, keys, options))

    DELEGATE_TIMESTAMPED_METHOD(TFuture<TSelectRowsResult>, SelectRows, (
        const TString& query,
        const TSelectRowsOptions& options),
        (query, options))

    DELEGATE_TIMESTAMPED_METHOD(TFuture<NYson::TYsonString>, Explain, (
        const TString& query,
        const TExplainOptions& options),
        (query, options))

    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, GetNode, (
        const TYPath& path,
        const TGetNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, SetNode, (
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options),
        (path, value, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, RemoveNode, (
        const TYPath& path,
        const TRemoveNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, ListNode, (
        const TYPath& path,
        const TListNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CreateNode, (
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options),
        (path, type, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TLockNodeResult>, LockNode, (
        const TYPath& path,
        NCypressClient::ELockMode mode,
        const TLockNodeOptions& options),
        (path, mode, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, UnlockNode, (
        const TYPath& path,
        const TUnlockNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CopyNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, MoveNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, LinkNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, ConcatenateNodes, (
        const std::vector<TRichYPath>& srcPaths,
        const TRichYPath& dstPath,
        const TConcatenateNodesOptions& options),
        (srcPaths, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, ExternalizeNode, (
        const TYPath& path,
        TCellTag cellTag,
        const TExternalizeNodeOptions& options),
        (path, cellTag, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, InternalizeNode, (
        const TYPath& path,
        const TInternalizeNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<bool>, NodeExists, (
        const TYPath& path,
        const TNodeExistsOptions& options),
        (path, options))


    DELEGATE_METHOD(TFuture<TObjectId>, CreateObject, (
        EObjectType type,
        const TCreateObjectOptions& options),
        (type, options))


    DELEGATE_TRANSACTIONAL_METHOD(TFuture<IFileReaderPtr>, CreateFileReader, (
        const TYPath& path,
        const TFileReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IFileWriterPtr, CreateFileWriter, (
        const TRichYPath& path,
        const TFileWriterOptions& options),
        (path, options))


    DELEGATE_TRANSACTIONAL_METHOD(IJournalReaderPtr, CreateJournalReader, (
        const TYPath& path,
        const TJournalReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IJournalWriterPtr, CreateJournalWriter, (
        const TYPath& path,
        const TJournalWriterOptions& options),
        (path, options))

    DELEGATE_TRANSACTIONAL_METHOD(TFuture<ITableReaderPtr>, CreateTableReader, (
        const TRichYPath& path,
        const TTableReaderOptions& options),
        (path, options))

    DELEGATE_TRANSACTIONAL_METHOD(TFuture<ITableWriterPtr>, CreateTableWriter, (
        const TRichYPath& path,
        const TTableWriterOptions& options),
        (path, options))

#undef DELEGATE_TRANSACTIONAL_METHOD
#undef DELEGATE_TIMESTAMPED_METHOD

private:
    const IClientPtr Client_;
    const NTransactionClient::TTransactionPtr Transaction_;

    const NLogging::TLogger Logger;

    struct TNativeTransactionBufferTag
    { };

    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TNativeTransactionBufferTag());

    TSpinLock SpinLock_;
    ETransactionState State_ = ETransactionState::Active;
    bool Prepared_ = false;
    TFuture<void> AbortResult_;

    TSpinLock ForeignTransactionsLock_;
    std::vector<NApi::ITransactionPtr> ForeignTransactions_;


    class TTableCommitSession;
    using TTableCommitSessionPtr = TIntrusivePtr<TTableCommitSession>;

    class TTabletCommitSession;
    using TTabletCommitSessionPtr = TIntrusivePtr<TTabletCommitSession>;

    class TCellCommitSession;
    using TCellCommitSessionPtr = TIntrusivePtr<TCellCommitSession>;


    class TModificationRequest
    {
    public:
        TModificationRequest(
            TTransaction* transaction,
            IConnectionPtr connection,
            const TYPath& path,
            TNameTablePtr nameTable,
            TSharedRange<TRowModification> modifications,
            const TModifyRowsOptions& options)
            : Transaction_(transaction)
            , Connection_(std::move(connection))
            , Path_(path)
            , NameTable_(std::move(nameTable))
            , Modifications_(std::move(modifications))
            , Options_(options)
            , Logger(Transaction_->Logger)
        { }

        std::optional<i64> GetSequenceNumber()
        {
            return Options_.SequenceNumber;
        }

        void PrepareTableSessions()
        {
            TableSession_ = Transaction_->GetOrCreateTableSession(Path_, Options_.UpstreamReplicaId);
        }

        void SubmitRows()
        {
            const auto& tableInfo = TableSession_->GetInfo();
            if (Options_.UpstreamReplicaId && tableInfo->IsReplicated()) {
                THROW_ERROR_EXCEPTION("Replicated table %v cannot act as a replication sink",
                    tableInfo->Path);
            }

            if (!tableInfo->Replicas.empty() &&
                TableSession_->SyncReplicas().empty() &&
                Options_.RequireSyncReplica)
            {
                THROW_ERROR_EXCEPTION("Table %v has no synchronous replicas",
                    tableInfo->Path);
            }

            for (const auto& replicaData : TableSession_->SyncReplicas()) {
                auto replicaOptions = Options_;
                replicaOptions.UpstreamReplicaId = replicaData.ReplicaInfo->ReplicaId;
                replicaOptions.SequenceNumber.reset();
                if (replicaData.Transaction) {
                    YT_LOG_DEBUG("Submitting remote sync replication modifications (Count: %v)",
                        Modifications_.Size());
                    replicaData.Transaction->ModifyRows(
                        replicaData.ReplicaInfo->ReplicaPath,
                        NameTable_,
                        Modifications_,
                        replicaOptions);
                } else {
                    // YT-7551: Local sync replicas must be handled differenly.
                    // We cannot add more modifications via ITransactions interface since
                    // the transaction is already committing.
                    YT_LOG_DEBUG("Buffering local sync replication modifications (Count: %v)",
                        Modifications_.Size());
                    Transaction_->EnqueueModificationRequest(std::make_unique<TModificationRequest>(
                        Transaction_,
                        Connection_,
                        replicaData.ReplicaInfo->ReplicaPath,
                        NameTable_,
                        Modifications_,
                        replicaOptions));
                }
            }

            std::optional<int> tabletIndexColumnId;
            if (!tableInfo->IsSorted()) {
                tabletIndexColumnId = NameTable_->GetIdOrRegisterName(TabletIndexColumnName);
            }

            const auto& primarySchema = tableInfo->Schemas[ETableSchemaKind::Primary];
            const auto& primaryIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::Primary);

            const auto& primarySchemaWithTabletIndex = tableInfo->Schemas[ETableSchemaKind::PrimaryWithTabletIndex];
            const auto& primaryWithTabletIndexIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::PrimaryWithTabletIndex);

            const auto& writeSchema = tableInfo->Schemas[ETableSchemaKind::Write];
            const auto& writeIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::Write);

            const auto& versionedWriteSchema = tableInfo->Schemas[ETableSchemaKind::VersionedWrite];
            const auto& versionedWriteIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::VersionedWrite);

            const auto& deleteSchema = tableInfo->Schemas[ETableSchemaKind::Delete];
            const auto& deleteIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::Delete);

            const auto& modificationSchema = !tableInfo->IsReplicated() && !tableInfo->IsSorted() ? primarySchema : primarySchemaWithTabletIndex;
            const auto& modificationIdMapping = !tableInfo->IsReplicated() && !tableInfo->IsSorted() ? primaryIdMapping : primaryWithTabletIndexIdMapping;

            const auto& rowBuffer = Transaction_->RowBuffer_;

            auto evaluatorCache = Connection_->GetColumnEvaluatorCache();
            auto evaluator = tableInfo->NeedKeyEvaluation ? evaluatorCache->Find(primarySchema) : nullptr;

            auto randomTabletInfo = tableInfo->GetRandomMountedTablet();

            std::vector<bool> columnPresenceBuffer(modificationSchema.GetColumnCount());

            // FLS slots are reused, so we need to manually reset the reporter.
            EntityInAnyReporter.Reset();

            for (const auto& modification : Modifications_) {
                switch (modification.Type) {
                    case ERowModificationType::Write:
                        ValidateClientDataRow(
                            TUnversionedRow(modification.Row),
                            writeSchema,
                            writeIdMapping,
                            NameTable_,
                            tabletIndexColumnId);
                        break;

                    case ERowModificationType::VersionedWrite:
                        if (!tableInfo->IsSorted()) {
                            THROW_ERROR_EXCEPTION("Cannot perform versioned writes into a non-sorted table %v",
                                tableInfo->Path);
                        }
                        if (tableInfo->IsReplicated()) {
                            THROW_ERROR_EXCEPTION("Cannot perform versioned writes into a replicated table %v",
                                tableInfo->Path);
                        }
                        ValidateClientDataRow(
                            TVersionedRow(modification.Row),
                            versionedWriteSchema,
                            versionedWriteIdMapping,
                            NameTable_);
                        break;

                    case ERowModificationType::Delete:
                        if (!tableInfo->IsSorted()) {
                            THROW_ERROR_EXCEPTION("Cannot perform deletes in a non-sorted table %v",
                                tableInfo->Path);
                        }
                        ValidateClientKey(
                            TUnversionedRow(modification.Row),
                            deleteSchema,
                            deleteIdMapping,
                            NameTable_);
                        break;

                    case ERowModificationType::ReadLockWrite:
                        if (!tableInfo->IsSorted()) {
                            THROW_ERROR_EXCEPTION("Cannot perform lock in a non-sorted table %v",
                                tableInfo->Path);
                        }
                        ValidateClientKey(
                            TUnversionedRow(modification.Row),
                            deleteSchema,
                            deleteIdMapping,
                            NameTable_);
                        break;

                    default:
                        YT_ABORT();
                }

                switch (modification.Type) {
                    case ERowModificationType::Write:
                    case ERowModificationType::Delete:
                    case ERowModificationType::ReadLockWrite: {
                        auto capturedRow = rowBuffer->CaptureAndPermuteRow(
                            TUnversionedRow(modification.Row),
                            modificationSchema,
                            modificationIdMapping,
                            modification.Type == ERowModificationType::Write ? &columnPresenceBuffer : nullptr);
                        TTabletInfoPtr tabletInfo;
                        if (tableInfo->IsSorted()) {
                            if (evaluator) {
                                evaluator->EvaluateKeys(capturedRow, rowBuffer);
                            }
                            tabletInfo = GetSortedTabletForRow(tableInfo, capturedRow, true);
                        } else {
                            tabletInfo = GetOrderedTabletForRow(
                                tableInfo,
                                randomTabletInfo,
                                tabletIndexColumnId,
                                TUnversionedRow(modification.Row),
                                true);
                        }
                        auto session = Transaction_->GetOrCreateTabletSession(tabletInfo, tableInfo, TableSession_);
                        auto command = GetCommand(modification.Type);
                        session->SubmitRow(command, capturedRow, modification.Locks);
                        break;
                    }

                    case ERowModificationType::VersionedWrite: {
                        auto capturedRow = rowBuffer->CaptureAndPermuteRow(
                            TVersionedRow(modification.Row),
                            primarySchema,
                            primaryIdMapping,
                            &columnPresenceBuffer);
                        if (evaluator) {
                            evaluator->EvaluateKeys(capturedRow, rowBuffer);
                        }
                        auto tabletInfo = GetSortedTabletForRow(tableInfo, capturedRow, true);
                        auto session = Transaction_->GetOrCreateTabletSession(tabletInfo, tableInfo, TableSession_);
                        session->SubmitRow(capturedRow);
                        break;
                    }

                    default:
                        YT_ABORT();
                }
            }
        }

    protected:
        TTransaction* const Transaction_;
        const IConnectionPtr Connection_;
        const TYPath Path_;
        const TNameTablePtr NameTable_;
        const TSharedRange<TRowModification> Modifications_;
        const TModifyRowsOptions Options_;

        const NLogging::TLogger& Logger;

        TTableCommitSessionPtr TableSession_;


        static EWireProtocolCommand GetCommand(ERowModificationType modificationType)
        {
            switch (modificationType) {
                case ERowModificationType::Write:
                    return EWireProtocolCommand::WriteRow;

                case ERowModificationType::VersionedWrite:
                    return EWireProtocolCommand::VersionedWriteRow;

                case ERowModificationType::Delete:
                    return EWireProtocolCommand::DeleteRow;

                case ERowModificationType::ReadLockWrite:
                    return EWireProtocolCommand::ReadLockWriteRow;

                default:
                    YT_ABORT();
            }
        }
    };

    std::vector<std::unique_ptr<TModificationRequest>> Requests_;
    std::vector<TModificationRequest*> PendingRequests_;
    TSlidingWindow<TModificationRequest*> OrderedRequestsSlidingWindow_;

    struct TSyncReplica
    {
        TTableReplicaInfoPtr ReplicaInfo;
        NApi::ITransactionPtr Transaction;
    };

    class TTableCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        TTableCommitSession(
            TTransaction* transaction,
            TTableMountInfoPtr tableInfo,
            TTableReplicaId upstreamReplicaId)
            : Transaction_(transaction)
            , TableInfo_(std::move(tableInfo))
            , UpstreamReplicaId_(upstreamReplicaId)
            , Logger(NLogging::TLogger(transaction->Logger)
                .AddTag("Path: %v", TableInfo_->Path))
        { }

        const TTableMountInfoPtr& GetInfo() const
        {
            return TableInfo_;
        }

        TTableReplicaId GetUpstreamReplicaId() const
        {
            return UpstreamReplicaId_;
        }

        const std::vector<TSyncReplica>& SyncReplicas() const
        {
            return SyncReplicas_;
        }


        void RegisterSyncReplicas(bool* clusterDirectorySynced)
        {
            for (const auto& replicaInfo : TableInfo_->Replicas) {
                if (replicaInfo->Mode != ETableReplicaMode::Sync) {
                    continue;
                }

                YT_LOG_DEBUG("Sync table replica registered (ReplicaId: %v, ClusterName: %v, ReplicaPath: %v)",
                    replicaInfo->ReplicaId,
                    replicaInfo->ClusterName,
                    replicaInfo->ReplicaPath);

                auto syncReplicaTransaction = Transaction_->GetSyncReplicaTransaction(
                    replicaInfo,
                    clusterDirectorySynced);
                SyncReplicas_.push_back(TSyncReplica{replicaInfo, std::move(syncReplicaTransaction)});
            }
        }

    private:
        TTransaction* const Transaction_;
        const TTableMountInfoPtr TableInfo_;
        const TTableReplicaId UpstreamReplicaId_;
        const NLogging::TLogger Logger;

        std::vector<TSyncReplica> SyncReplicas_;

    };

    //! Maintains per-table commit info.
    THashMap<TYPath, TTableCommitSessionPtr> TablePathToSession_;
    std::vector<TTableCommitSessionPtr> PendingSessions_;

    class TTabletCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        TTabletCommitSession(
            TTransactionPtr transaction,
            TTabletInfoPtr tabletInfo,
            TTableMountInfoPtr tableInfo,
            TTableCommitSessionPtr tableSession,
            TColumnEvaluatorPtr columnEvaluator)
            : Transaction_(transaction)
            , TableInfo_(std::move(tableInfo))
            , TabletInfo_(std::move(tabletInfo))
            , TableSession_(std::move(tableSession))
            , Config_(transaction->Client_->GetNativeConnection()->GetConfig())
            , UserName_(transaction->Client_->GetOptions().GetUser())
            , ColumnEvaluator_(std::move(columnEvaluator))
            , TableMountCache_(transaction->Client_->GetNativeConnection()->GetTableMountCache())
            , ColumnCount_(TableInfo_->Schemas[ETableSchemaKind::Primary].Columns().size())
            , KeyColumnCount_(TableInfo_->Schemas[ETableSchemaKind::Primary].GetKeyColumnCount())
            , Logger(NLogging::TLogger(transaction->Logger)
                .AddTag("TabletId: %v", TabletInfo_->TabletId))
        { }

        void SubmitRow(
            EWireProtocolCommand command,
            TUnversionedRow row,
            TLockMask lockMask)
        {
            UnversionedSubmittedRows_.push_back({
                command,
                row,
                lockMask,
                static_cast<int>(UnversionedSubmittedRows_.size())});
        }

        void SubmitRow(TVersionedRow row)
        {
            VersionedSubmittedRows_.push_back(row);
        }

        int Prepare()
        {
            if (!VersionedSubmittedRows_.empty() && !UnversionedSubmittedRows_.empty()) {
                THROW_ERROR_EXCEPTION("Cannot intermix versioned and unversioned writes to a single table "
                    "within a transaction");
            }

            if (TableInfo_->IsSorted()) {
                PrepareSortedBatches();
            } else {
                PrepareOrderedBatches();
            }

            return static_cast<int>(Batches_.size());
        }

        TFuture<void> Invoke(IChannelPtr channel)
        {
            // Do all the heavy lifting here.
            auto* codec = NCompression::GetCodec(Config_->WriteRowsRequestCodec);
            YT_VERIFY(!Batches_.empty());
            for (const auto& batch : Batches_) {
                batch->RequestData = codec->Compress(batch->Writer.Finish());
            }

            InvokeChannel_ = channel;
            InvokeNextBatch();
            return InvokePromise_;
        }

        TCellId GetCellId() const
        {
            return TabletInfo_->CellId;
        }

    private:
        const TWeakPtr<TTransaction> Transaction_;
        const TTableMountInfoPtr TableInfo_;
        const TTabletInfoPtr TabletInfo_;
        const TTableCommitSessionPtr TableSession_;
        const TConnectionConfigPtr Config_;
        const TString UserName_;
        const TColumnEvaluatorPtr ColumnEvaluator_;
        const ITableMountCachePtr TableMountCache_;
        const int ColumnCount_;
        const int KeyColumnCount_;

        struct TCommitSessionBufferTag
        { };

        TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TCommitSessionBufferTag());

        NLogging::TLogger Logger;

        struct TBatch
        {
            TWireProtocolWriter Writer;
            TSharedRef RequestData;
            int RowCount = 0;
            size_t DataWeight = 0;
        };

        int TotalBatchedRowCount_ = 0;
        std::vector<std::unique_ptr<TBatch>> Batches_;

        std::vector<TVersionedRow> VersionedSubmittedRows_;

        struct TUnversionedSubmittedRow
        {
            EWireProtocolCommand Command;
            TUnversionedRow Row;
            TLockMask Locks;
            int SequentialId;
        };

        std::vector<TUnversionedSubmittedRow> UnversionedSubmittedRows_;

        IChannelPtr InvokeChannel_;
        int InvokeBatchIndex_ = 0;
        TPromise<void> InvokePromise_ = NewPromise<void>();

        void PrepareSortedBatches()
        {
            std::sort(
                UnversionedSubmittedRows_.begin(),
                UnversionedSubmittedRows_.end(),
                [=] (const TUnversionedSubmittedRow& lhs, const TUnversionedSubmittedRow& rhs) {
                    // NB: CompareRows may throw on composite values.
                    int res = CompareRows(lhs.Row, rhs.Row, KeyColumnCount_);
                    return res != 0 ? res < 0 : lhs.SequentialId < rhs.SequentialId;
                });

            std::vector<TUnversionedSubmittedRow> unversionedMergedRows;
            unversionedMergedRows.reserve(UnversionedSubmittedRows_.size());

            TUnversionedRowMerger merger(
                RowBuffer_,
                ColumnCount_,
                KeyColumnCount_,
                ColumnEvaluator_);

            for (auto it = UnversionedSubmittedRows_.begin(); it != UnversionedSubmittedRows_.end();) {
                auto startIt = it;
                merger.InitPartialRow(startIt->Row);

                TLockMask lockMask;
                EWireProtocolCommand resultCommand;

                do {
                    switch (it->Command) {
                        case EWireProtocolCommand::DeleteRow:
                            merger.DeletePartialRow(it->Row);
                            break;

                        case EWireProtocolCommand::WriteRow:
                            merger.AddPartialRow(it->Row);
                            break;

                        case EWireProtocolCommand::ReadLockWriteRow:
                            merger.AddPartialRow(it->Row);
                            lockMask = MaxMask(lockMask, it->Locks);
                            break;

                        default:
                            YT_ABORT();
                    }
                    resultCommand = it->Command;
                    ++it;
                } while (it != UnversionedSubmittedRows_.end() &&
                    CompareRows(it->Row, startIt->Row, KeyColumnCount_) == 0);

                TUnversionedRow mergedRow;
                if (resultCommand == EWireProtocolCommand::DeleteRow) {
                    mergedRow = merger.BuildDeleteRow();
                } else {
                    if (lockMask) {
                        resultCommand = EWireProtocolCommand::ReadLockWriteRow;
                    }
                    mergedRow = merger.BuildMergedRow();
                }

                unversionedMergedRows.push_back({resultCommand, mergedRow, lockMask});
            }

            for (const auto& submittedRow : unversionedMergedRows) {
                WriteRow(submittedRow);
            }

            for (const auto& row : VersionedSubmittedRows_) {

                IncrementAndCheckRowCount();

                auto* batch = EnsureBatch();
                auto& writer = batch->Writer;
                ++batch->RowCount;
                batch->DataWeight += GetDataWeight(row);

                writer.WriteCommand(EWireProtocolCommand::VersionedWriteRow);
                writer.WriteVersionedRow(row);
            }
        }

        void WriteRow(const TUnversionedSubmittedRow& submittedRow)
        {
            IncrementAndCheckRowCount();

            auto* batch = EnsureBatch();
            auto& writer = batch->Writer;
            ++batch->RowCount;
            batch->DataWeight += GetDataWeight(submittedRow.Row);

            writer.WriteCommand(submittedRow.Command);

            if (submittedRow.Command == EWireProtocolCommand::ReadLockWriteRow) {
                writer.WriteLockBitmap(submittedRow.Locks);
            }

            writer.WriteUnversionedRow(submittedRow.Row);
        }

        void PrepareOrderedBatches()
        {
            for (const auto& submittedRow : UnversionedSubmittedRows_) {
                WriteRow(submittedRow);
            }
        }

        bool IsNewBatchNeeded()
        {
            if (Batches_.empty()) {
                return true;
            }

            const auto& lastBatch = Batches_.back();
            if (lastBatch->RowCount >= Config_->MaxRowsPerWriteRequest) {
                return true;
            }
            if (lastBatch->DataWeight >= Config_->MaxDataWeightPerWriteRequest) {
                return true;
            }

            return false;
        }

        TBatch* EnsureBatch()
        {
            if (IsNewBatchNeeded()) {
                Batches_.emplace_back(new TBatch());
            }
            return Batches_.back().get();
        }

        void IncrementAndCheckRowCount()
        {
            ++TotalBatchedRowCount_;
            if (UserName_ != NSecurityClient::ReplicatorUserName &&
                TotalBatchedRowCount_ > Config_->MaxRowsPerTransaction)
            {
                THROW_ERROR_EXCEPTION("Transaction affects too many rows")
                    << TErrorAttribute("limit", Config_->MaxRowsPerTransaction);
            }
        }

        void InvokeNextBatch()
        {
            if (InvokeBatchIndex_ >= Batches_.size()) {
                InvokePromise_.Set(TError());
                return;
            }

            const auto& batch = Batches_[InvokeBatchIndex_++];

            auto transaction = Transaction_.Lock();
            if (!transaction) {
                return;
            }

            auto cellSession = transaction->GetCommitSession(GetCellId());

            TTabletServiceProxy proxy(InvokeChannel_);
            proxy.SetDefaultTimeout(Config_->WriteRowsTimeout);
            proxy.SetDefaultRequestAck(false);

            auto req = proxy.Write();
            req->SetMultiplexingBand(EMultiplexingBand::Heavy);
            ToProto(req->mutable_transaction_id(), transaction->GetId());
            if (transaction->GetAtomicity() == EAtomicity::Full) {
                req->set_transaction_start_timestamp(transaction->GetStartTimestamp());
                req->set_transaction_timeout(ToProto<i64>(transaction->GetTimeout()));
            }
            ToProto(req->mutable_tablet_id(), TabletInfo_->TabletId);
            req->set_mount_revision(TabletInfo_->MountRevision);
            req->set_durability(static_cast<int>(transaction->GetDurability()));
            req->set_signature(cellSession->AllocateRequestSignature());
            req->set_request_codec(static_cast<int>(Config_->WriteRowsRequestCodec));
            req->set_row_count(batch->RowCount);
            req->set_data_weight(batch->DataWeight);
            req->set_versioned(!VersionedSubmittedRows_.empty());
            for (const auto& replicaInfo : TableInfo_->Replicas) {
                if (replicaInfo->Mode == ETableReplicaMode::Sync) {
                    ToProto(req->add_sync_replica_ids(), replicaInfo->ReplicaId);
                }
            }
            if (TableSession_->GetUpstreamReplicaId()) {
                ToProto(req->mutable_upstream_replica_id(), TableSession_->GetUpstreamReplicaId());
            }
            req->Attachments().push_back(batch->RequestData);

            YT_LOG_DEBUG("Sending transaction rows (BatchIndex: %v/%v, RowCount: %v, Signature: %x, "
                "Versioned: %v, UpstreamReplicaId: %v)",
                InvokeBatchIndex_,
                Batches_.size(),
                batch->RowCount,
                req->signature(),
                req->versioned(),
                TableSession_->GetUpstreamReplicaId());

            // NB: OnResponse is trivial for the last batch; otherwise use thread pool invoker.
            auto invoker = InvokeBatchIndex_ == Batches_.size()
                ? GetSyncInvoker()
                : transaction->GetThreadPoolInvoker();
            req->Invoke().Subscribe(
                BIND(&TTabletCommitSession::OnResponse, MakeStrong(this))
                    .Via(std::move(invoker)));
        }

        void OnResponse(const TTabletServiceProxy::TErrorOrRspWritePtr& rspOrError)
        {
            if (!rspOrError.IsOK()) {
                auto error = TError("Error sending transaction rows")
                    << rspOrError;
                YT_LOG_DEBUG(error);
                TableMountCache_->InvalidateOnError(error);
                InvokePromise_.Set(error);
                return;
            }

            auto owner = Transaction_.Lock();
            if (!owner) {
                return;
            }

            YT_LOG_DEBUG("Transaction rows sent successfully (BatchIndex: %v/%v)",
                InvokeBatchIndex_,
                Batches_.size());

            owner->Transaction_->ConfirmParticipant(TabletInfo_->CellId);
            InvokeNextBatch();
        }
    };

    //! Maintains per-tablet commit info.
    THashMap<TTabletId, TTabletCommitSessionPtr> TabletIdToSession_;

    class TCellCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        TCellCommitSession(const TTransactionPtr& transaction, TCellId cellId)
            : Transaction_(transaction)
            , CellId_(cellId)
            , Logger(NLogging::TLogger(transaction->Logger)
                .AddTag("CellId: %v", CellId_))
        { }

        void RegisterRequests(int count)
        {
            VERIFY_THREAD_AFFINITY_ANY();

            RequestsTotal_ += count;
            RequestsRemaining_ += count;
        }

        TTransactionSignature AllocateRequestSignature()
        {
            VERIFY_THREAD_AFFINITY_ANY();

            auto remaining = --RequestsRemaining_;
            YT_VERIFY(remaining >= 0);
            return remaining == 0
                ? FinalTransactionSignature - InitialTransactionSignature - RequestsTotal_.load() + 1
                : 1;
        }

        void RegisterAction(const TTransactionActionData& data)
        {
            if (Actions_.empty()) {
                RegisterRequests(1);
            }
            Actions_.push_back(data);
        }

        TFuture<void> Invoke(const IChannelPtr& channel)
        {
            if (Actions_.empty()) {
                return VoidFuture;
            }

            auto transaction = Transaction_.Lock();
            if (!transaction) {
                return MakeFuture(TError("Transaction is no longer available"));
            }

            YT_LOG_DEBUG("Sending transaction actions (ActionCount: %v)",
                Actions_.size());

            TFuture<void> asyncResult;
            switch (TypeFromId(CellId_)) {
                case EObjectType::TabletCell:
                    asyncResult = SendTabletActions(transaction, channel);
                    break;
                case EObjectType::ClusterCell:
                    asyncResult = SendMasterActions(transaction, channel);
                    break;
                default:
                    YT_ABORT();
            }

            return asyncResult.Apply(
                // NB: OnResponse is trivial; need no invoker here.
                BIND(&TCellCommitSession::OnResponse, MakeStrong(this)));
        }

    private:
        const TWeakPtr<TTransaction> Transaction_;
        const TCellId CellId_;
        const NLogging::TLogger Logger;

        std::vector<TTransactionActionData> Actions_;

        std::atomic<int> RequestsTotal_ = {0};
        std::atomic<int> RequestsRemaining_ = {0};


        TFuture<void> SendTabletActions(const TTransactionPtr& owner, const IChannelPtr& channel)
        {
            TTabletServiceProxy proxy(channel);
            auto req = proxy.RegisterTransactionActions();
            ToProto(req->mutable_transaction_id(), owner->GetId());
            req->set_transaction_start_timestamp(owner->GetStartTimestamp());
            req->set_transaction_timeout(ToProto<i64>(owner->GetTimeout()));
            req->set_signature(AllocateRequestSignature());
            ToProto(req->mutable_actions(), Actions_);
            return req->Invoke().As<void>();
        }

        TFuture<void> SendMasterActions(const TTransactionPtr& owner, const IChannelPtr& channel)
        {
            TTransactionServiceProxy proxy(channel);
            auto req = proxy.RegisterTransactionActions();
            ToProto(req->mutable_transaction_id(), owner->GetId());
            ToProto(req->mutable_actions(), Actions_);
            return req->Invoke().As<void>();
        }

        void OnResponse(const TError& result)
        {
            if (!result.IsOK()) {
                auto error = TError("Error sending transaction actions")
                    << result;
                YT_LOG_DEBUG(error);
                THROW_ERROR(error);
            }

            auto transaction = Transaction_.Lock();
            if (!transaction) {
                THROW_ERROR_EXCEPTION("Transaction is no longer available");
            }

            if (TypeFromId(CellId_) == EObjectType::TabletCell) {
                transaction->Transaction_->ConfirmParticipant(CellId_);
            }

            YT_LOG_DEBUG("Transaction actions sent successfully");
        }
    };

    //! Maintains per-cell commit info.
    THashMap<TCellId, TCellCommitSessionPtr> CellIdToSession_;

    //! Maps replica cluster name to sync replica transaction.
    THashMap<TString, NApi::ITransactionPtr> ClusterNameToSyncReplicaTransaction_;

    //! Caches mappings from name table ids to schema ids.
    THashMap<std::tuple<TTableId, TNameTablePtr, ETableSchemaKind>, TNameTableToSchemaIdMapping> IdMappingCache_;


    IInvokerPtr GetThreadPoolInvoker()
    {
        return Client_->GetConnection()->GetInvoker();
    }

    const TNameTableToSchemaIdMapping& GetColumnIdMapping(
        const TTableMountInfoPtr& tableInfo,
        const TNameTablePtr& nameTable,
        ETableSchemaKind kind)
    {
        auto key = std::make_tuple(tableInfo->TableId, nameTable, kind);
        auto it = IdMappingCache_.find(key);
        if (it == IdMappingCache_.end()) {
            auto mapping = BuildColumnIdMapping(tableInfo->Schemas[kind], nameTable);
            it = IdMappingCache_.emplace(key, std::move(mapping)).first;
        }
        return it->second;
    }

    NApi::ITransactionPtr GetSyncReplicaTransaction(
        const TTableReplicaInfoPtr& replicaInfo,
        bool* clusterDirectorySynced)
    {
        auto it = ClusterNameToSyncReplicaTransaction_.find(replicaInfo->ClusterName);
        if (it != ClusterNameToSyncReplicaTransaction_.end()) {
            return it->second;
        }

        const auto& clusterDirectory = Client_->GetNativeConnection()->GetClusterDirectory();
        auto connection = clusterDirectory->FindConnection(replicaInfo->ClusterName);
        if (!connection) {
            if (!*clusterDirectorySynced) {
                YT_LOG_DEBUG("Replica cluster is not known; synchronizing cluster directory");
                WaitFor(Client_->GetNativeConnection()->GetClusterDirectorySynchronizer()->Sync())
                    .ThrowOnError();
                *clusterDirectorySynced = true;
            }
            connection = clusterDirectory->GetConnectionOrThrow(replicaInfo->ClusterName);
        }

        if (connection->GetCellTag() == Client_->GetConnection()->GetCellTag()) {
            return nullptr;
        }

        auto client = connection->CreateClient(Client_->GetOptions());

        TForeignTransactionStartOptions options;
        options.InheritStartTimestamp = true;
        auto transaction = WaitFor(StartForeignTransaction(client, options))
            .ValueOrThrow();

        YT_VERIFY(ClusterNameToSyncReplicaTransaction_.emplace(replicaInfo->ClusterName, transaction).second);

        YT_LOG_DEBUG("Sync replica transaction started (ClusterName: %v)",
            replicaInfo->ClusterName);

        return transaction;
    }

    void DoEnqueueModificationRequest(TModificationRequest* request)
    {
        PendingRequests_.push_back(request);
    }


    void EnqueueModificationRequest(
        std::unique_ptr<TModificationRequest> request)
    {
        try {
            GuardedEnqueueModificationRequest(std::move(request));
        } catch (const std::exception& ex) {
            State_ = ETransactionState::Abort;
            Transaction_->Abort();
            // TODO(kiselyovp) abort foreign transactions?
            throw;
        }
    }

    void GuardedEnqueueModificationRequest(
        std::unique_ptr<TModificationRequest> request)
    {
        auto sequenceNumber = request->GetSequenceNumber();

        if (sequenceNumber) {
            if (*sequenceNumber < 0) {
                THROW_ERROR_EXCEPTION("Packet sequence number is negative")
                    << TErrorAttribute("sequence_number", *sequenceNumber);
            }
            // this may call DoEnqueueModificationRequest right away
            OrderedRequestsSlidingWindow_.AddPacket(*sequenceNumber, request.get(), [&] (TModificationRequest* request) {
                DoEnqueueModificationRequest(request);
            });
        } else {
            DoEnqueueModificationRequest(request.get());
        }
        Requests_.push_back(std::move(request));
    }

    TTableCommitSessionPtr GetOrCreateTableSession(const TYPath& path, TTableReplicaId upstreamReplicaId)
    {
        auto it = TablePathToSession_.find(path);
        if (it == TablePathToSession_.end()) {
            const auto& tableMountCache = Client_->GetTableMountCache();
            auto tableInfo = WaitFor(tableMountCache->GetTableInfo(path))
                .ValueOrThrow();

            auto session = New<TTableCommitSession>(this, std::move(tableInfo), upstreamReplicaId);
            PendingSessions_.push_back(session);
            it = TablePathToSession_.emplace(path, session).first;
        } else {
            const auto& session = it->second;
            if (session->GetUpstreamReplicaId() != upstreamReplicaId) {
                THROW_ERROR_EXCEPTION("Mismatched upstream replica is specified for modifications to table %v: %v != !v",
                    path,
                    upstreamReplicaId,
                    session->GetUpstreamReplicaId());
            }
        }
        return it->second;
    }

    TTabletCommitSessionPtr GetOrCreateTabletSession(
        const TTabletInfoPtr& tabletInfo,
        const TTableMountInfoPtr& tableInfo,
        const TTableCommitSessionPtr& tableSession)
    {
        auto tabletId = tabletInfo->TabletId;
        auto it = TabletIdToSession_.find(tabletId);
        if (it == TabletIdToSession_.end()) {
            auto evaluatorCache = Client_->GetNativeConnection()->GetColumnEvaluatorCache();
            auto evaluator = evaluatorCache->Find(tableInfo->Schemas[ETableSchemaKind::Primary]);
            it = TabletIdToSession_.emplace(
                tabletId,
                New<TTabletCommitSession>(
                    this,
                    tabletInfo,
                    tableInfo,
                    tableSession,
                    evaluator)
                ).first;
        }
        return it->second;
    }

    void PrepareRequests()
    {
        bool clusterDirectorySynced = false;

        if (!OrderedRequestsSlidingWindow_.IsEmpty()) {
            THROW_ERROR_EXCEPTION("Cannot prepare transaction %v since sequence number %v is missing",
                GetId(),
                OrderedRequestsSlidingWindow_.GetNextSequenceNumber());
        }

        // Tables with local sync replicas pose a problem since modifications in such tables
        // induce more modifications that need to be taken care of.
        // Here we iterate over requests and sessions until no more new items are added.
        while (!PendingRequests_.empty() || !PendingSessions_.empty()) {
            decltype(PendingRequests_) pendingRequests;
            std::swap(PendingRequests_, pendingRequests);

            for (auto* request : pendingRequests) {
                request->PrepareTableSessions();
            }

            decltype(PendingSessions_) pendingSessions;
            std::swap(PendingSessions_, pendingSessions);

            for (const auto& tableSession : pendingSessions) {
                tableSession->RegisterSyncReplicas(&clusterDirectorySynced);
            }

            for (auto* request : pendingRequests) {
                request->SubmitRows();
            }
        }

        for (const auto& pair : TabletIdToSession_) {
            const auto& tabletSession = pair.second;
            auto cellId = tabletSession->GetCellId();
            int requestCount = tabletSession->Prepare();
            auto cellSession = GetOrCreateCellCommitSession(cellId);
            cellSession->RegisterRequests(requestCount);
        }

        for (const auto& pair : CellIdToSession_) {
            auto cellId = pair.first;
            Transaction_->RegisterParticipant(cellId);
        }

        {
            auto guard = Guard(SpinLock_);
            if (State_ == ETransactionState::Abort) {
                THROW_ERROR_EXCEPTION("Cannot prepare since transaction %v is already in %Qlv state",
                    GetId(),
                    State_);
            }
            YT_VERIFY(State_ == ETransactionState::Prepare || State_ == ETransactionState::Commit);
            YT_VERIFY(!Prepared_);
            Prepared_ = true;
        }
    }

    TFuture<void> SendRequests()
    {
        YT_VERIFY(Prepared_);

        std::vector<TFuture<void>> asyncResults;

        for (const auto& pair : TabletIdToSession_) {
            const auto& session = pair.second;
            auto cellId = session->GetCellId();
            auto channel = Client_->GetCellChannelOrThrow(cellId);
            asyncResults.push_back(session->Invoke(std::move(channel)));
        }

        for (const auto& pair : CellIdToSession_) {
            auto cellId = pair.first;
            const auto& session = pair.second;
            auto channel = Client_->GetCellChannelOrThrow(cellId);
            asyncResults.push_back(session->Invoke(std::move(channel)));
        }

        return Combine(asyncResults);
    }

    TTransactionCommitOptions AdjustCommitOptions(TTransactionCommitOptions options)
    {
        for (const auto& pair : TablePathToSession_) {
            const auto& session = pair.second;
            if (session->GetInfo()->IsReplicated()) {
                options.Force2PC = true;
            }
            if (!session->SyncReplicas().empty()) {
                options.CoordinatorCellTag = Client_->GetNativeConnection()->GetPrimaryMasterCellTag();
            }
        }

        return options;
    }

    TTransactionCommitResult DoCommit(const TTransactionCommitOptions& options)
    {
        try {
            // Gather participants.
            {
                PrepareRequests();

                std::vector<TFuture<TTransactionPrepareResult>> asyncPrepareResults;
                for (const auto& transaction : GetForeignTransactions()) {
                    asyncPrepareResults.push_back(transaction->Prepare());
                }

                auto prepareResults = WaitFor(Combine(asyncPrepareResults))
                    .ValueOrThrow();

                for (const auto& prepareResult : prepareResults) {
                    for (auto cellId : prepareResult.ParticipantCellIds) {
                        Transaction_->RegisterParticipant(cellId);
                    }
                }
            }

            // Choose coordinator.
            auto adjustedOptions = AdjustCommitOptions(options);
            Transaction_->ChooseCoordinator(adjustedOptions);

            // Validate that all participants are available.
            WaitFor(Transaction_->ValidateNoDownedParticipants())
                .ThrowOnError();

            // Send transaction data.
            {
                std::vector<TFuture<void>> asyncRequestResults{
                    SendRequests()
                };

                std::vector<TFuture<TTransactionFlushResult>> asyncFlushResults;
                for (const auto& transaction : GetForeignTransactions()) {
                    asyncFlushResults.push_back(transaction->Flush());
                }

                auto flushResults = WaitFor(Combine(asyncFlushResults))
                    .ValueOrThrow();

                for (const auto& flushResult : flushResults) {
                    asyncRequestResults.push_back(flushResult.AsyncResult);
                    for (auto cellId : flushResult.ParticipantCellIds) {
                        Transaction_->ConfirmParticipant(cellId);
                    }
                }

                WaitFor(Combine(asyncRequestResults))
                    .ThrowOnError();
            }

            // Commit.
            {
                auto commitResult = WaitFor(Transaction_->Commit(adjustedOptions))
                    .ValueOrThrow();

                for (const auto& transaction : GetForeignTransactions()) {
                    transaction->Detach();
                }

                return commitResult;
            }
        } catch (const std::exception& ex) {
            // Fire and forget.
            Transaction_->Abort();
            for (const auto& transaction : GetForeignTransactions()) {
                transaction->Abort();
            }
            throw;
        }
    }

    TTransactionPrepareResult DoPrepare()
    {
        PrepareRequests();

        TTransactionPrepareResult result;
        result.ParticipantCellIds = GetKeys(CellIdToSession_);
        return result;
    }

    TTransactionFlushResult DoFlush()
    {
        auto asyncResult = SendRequests();
        asyncResult.Subscribe(BIND([transaction = Transaction_] (const TError& error) {
            if (!error.IsOK()) {
                transaction->Abort();
            }
        }));

        TTransactionFlushResult result;
        result.AsyncResult = asyncResult;
        result.ParticipantCellIds = GetKeys(CellIdToSession_);
        return result;
    }


    TCellCommitSessionPtr GetOrCreateCellCommitSession(TCellId cellId)
    {
        auto it = CellIdToSession_.find(cellId);
        if (it == CellIdToSession_.end()) {
            it = CellIdToSession_.emplace(cellId, New<TCellCommitSession>(this, cellId)).first;
        }
        return it->second;
    }

    TCellCommitSessionPtr GetCommitSession(TCellId cellId)
    {
        return GetOrCrash(CellIdToSession_, cellId);
    }


    TTimestamp GetReadTimestamp() const
    {
        switch (Transaction_->GetAtomicity()) {
            case EAtomicity::Full:
                return GetStartTimestamp();
            case EAtomicity::None:
                // NB: Start timestamp is approximate.
                return SyncLastCommittedTimestamp;
            default:
                YT_ABORT();
        }
    }

    void RegisterForeignTransaction(NApi::ITransactionPtr transaction)
    {
        auto guard = Guard(ForeignTransactionsLock_);
        ForeignTransactions_.emplace_back(std::move(transaction));
    }

    std::vector<NApi::ITransactionPtr> GetForeignTransactions()
    {
        auto guard = Guard(ForeignTransactionsLock_);
        return ForeignTransactions_;
    }
};

DEFINE_REFCOUNTED_TYPE(TTransaction)

ITransactionPtr CreateTransaction(
    IClientPtr client,
    NTransactionClient::TTransactionPtr transaction,
    const NLogging::TLogger& logger)
{
    return New<TTransaction>(
        std::move(client),
        std::move(transaction),
        logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
