#pragma once

#include "public.h"
#include "connection.h"

#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/cypress_client/public.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/query_client/public.h>
#include <yt/ytlib/query_client/query_statistics.h>

#include <yt/ytlib/scheduler/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/row_base.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/ytlib/ypath/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/nullable.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/permission.h>

#include <yt/core/yson/string.h>

namespace NYT {
namespace NApi {

///////////////////////////////////////////////////////////////////////////////

struct TUserWorkloadDescriptor
{
    EUserWorkloadCategory Category = EUserWorkloadCategory::Realtime;
    int Band = 0;

    operator TWorkloadDescriptor() const;
};

void Serialize(const TUserWorkloadDescriptor& workloadDescriptor, NYson::IYsonConsumer* consumer);
void Deserialize(TUserWorkloadDescriptor& workloadDescriptor, NYTree::INodePtr node);

struct TTimeoutOptions
{
    TNullable<TDuration> Timeout;
};

struct TTabletRangeOptions
{
    TNullable<int> FirstTabletIndex;
    TNullable<int> LastTabletIndex;
};

struct TTransactionalOptions
{
    //! Ignored when queried via transaction.
    NObjectClient::TTransactionId TransactionId;
    bool Ping = false;
    bool PingAncestors = false;
    bool Sticky = false;
};

struct TSuppressableAccessTrackingOptions
{
    bool SuppressAccessTracking = false;
    bool SuppressModificationTracking = false;
};

struct TMutatingOptions
{
    NRpc::TMutationId MutationId;
    bool Retry = false;

    NRpc::TMutationId GetOrGenerateMutationId() const;
};

struct TMasterReadOptions
{
    EMasterChannelKind ReadFrom = EMasterChannelKind::Follower;
    TDuration ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(15);
    TDuration ExpireAfterFailedUpdateTime = TDuration::Seconds(15);
};

struct TPrerequisiteRevisionConfig
    : public NYTree::TYsonSerializable
{
    NYTree::TYPath Path;
    NTransactionClient::TTransactionId TransactionId;
    i64 Revision;

    TPrerequisiteRevisionConfig()
    {
        RegisterParameter("path", Path);
        RegisterParameter("transaction_id", TransactionId);
        RegisterParameter("revision", Revision);
    }
};

DEFINE_REFCOUNTED_TYPE(TPrerequisiteRevisionConfig)

struct TPrerequisiteOptions
{
    std::vector<NTransactionClient::TTransactionId> PrerequisiteTransactionIds;
    std::vector<TPrerequisiteRevisionConfigPtr> PrerequisiteRevisions;
};

struct TMountTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{
    NTabletClient::TTabletCellId CellId = NTabletClient::NullTabletCellId;
    bool Freeze = false;
};

struct TUnmountTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{
    bool Force = false;
};

struct TRemountTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{ };

struct TFreezeTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{ };

struct TUnfreezeTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{ };

struct TReshardTableOptions
    : public TTimeoutOptions
    , public TTabletRangeOptions
{ };

struct TAlterTableOptions
    : public TTimeoutOptions
    , public TMutatingOptions
    , public TTransactionalOptions
{
    TNullable<NTableClient::TTableSchema> Schema;
    TNullable<bool> Dynamic;
};

struct TTrimTableOptions
    : public TTimeoutOptions
{ };

struct TEnableTableReplicaOptions
    : public TTimeoutOptions
{ };

struct TDisableTableReplicaOptions
    : public TTimeoutOptions
{ };

struct TAddMemberOptions
    : public TTimeoutOptions
    , public TMutatingOptions
{ };

struct TRemoveMemberOptions
    : public TTimeoutOptions
    , public TMutatingOptions
{ };

struct TCheckPermissionOptions
    : public TTimeoutOptions
    , public TMasterReadOptions
    , public TTransactionalOptions
    , public TPrerequisiteOptions
{ };

struct TCheckPermissionResult
{
    TError ToError(const Stroka& user, NYTree::EPermission permission) const;

    NSecurityClient::ESecurityAction Action;
    NObjectClient::TObjectId ObjectId;
    TNullable<Stroka> ObjectName;
    NSecurityClient::TSubjectId SubjectId;
    TNullable<Stroka> SubjectName;
};

// TODO(lukyan): Use TTransactionalOptions as base class
struct TTransactionStartOptions
    : public TMutatingOptions
    , public TPrerequisiteOptions
{
    TNullable<TDuration> Timeout;
    //! If not null then the transaction must use this externally provided id.
    //! Only applicable to tablet trasactions.
    NTransactionClient::TTransactionId Id;
    NTransactionClient::TTransactionId ParentId;
    bool AutoAbort = true;
    bool Sticky = false;
    TNullable<TDuration> PingPeriod;
    bool Ping = true;
    bool PingAncestors = true;
    std::shared_ptr<const NYTree::IAttributeDictionary> Attributes;
    NTransactionClient::EAtomicity Atomicity = NTransactionClient::EAtomicity::Full;
    NTransactionClient::EDurability Durability = NTransactionClient::EDurability::Sync;
};

struct TTransactionAttachOptions
{
    bool AutoAbort = false;
    bool Sticky = false;
    TNullable<TDuration> PingPeriod;
    bool Ping = true;
    bool PingAncestors = false;
};

struct TTransactionCommitOptions
    : public TMutatingOptions
    , public TPrerequisiteOptions
    , public TTransactionalOptions
{ };

struct TTransactionAbortOptions
    : public TMutatingOptions
    , public TPrerequisiteOptions
    , public TTransactionalOptions
{
    bool Force = false;
};

struct TTabletReadOptions
{
    NHydra::EPeerKind ReadFrom = NHydra::EPeerKind::Leader;
    TNullable<TDuration> BackupRequestDelay;
};

struct TLookupRowsOptions
    : public TTimeoutOptions
    , public TTabletReadOptions
{
    NTableClient::TColumnFilter ColumnFilter;
    //! Ignored when queried via transaction.
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::SyncLastCommittedTimestamp;
    bool KeepMissingRows = false;
};

struct TSelectRowsOptions
    : public TTimeoutOptions
    , public TTabletReadOptions
{
    //! Ignored when queried via transaction.
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::SyncLastCommittedTimestamp;
    //! If null then connection defaults are used.
    TNullable<i64> InputRowLimit;
    //! If null then connection defaults are used.
    TNullable<i64> OutputRowLimit;
    //! Limits range expanding.
    ui64 RangeExpansionLimit = 1000;
    //! If |true| then incomplete result would lead to a failure.
    bool FailOnIncompleteResult = true;
    //! If |true| then logging is more verbose.
    bool VerboseLogging = false;
    //! Limits maximum parallel subqueries.
    int MaxSubqueries = std::numeric_limits<int>::max();
    //! Enables generated code caching.
    bool EnableCodeCache = true;
    //! Used to prioritize requests.
    TUserWorkloadDescriptor WorkloadDescriptor;
};

struct TGetNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMasterReadOptions
    , public TSuppressableAccessTrackingOptions
    , public TPrerequisiteOptions
{
    // XXX(sandello): This one is used only in ProfileManager to pass `from_time`.
    std::shared_ptr<const NYTree::IAttributeDictionary> Options;
    TNullable<std::vector<Stroka>> Attributes;
    TNullable<i64> MaxSize;
};

struct TSetNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{ };

struct TRemoveNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    bool Recursive = true;
    bool Force = false;
};

struct TListNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMasterReadOptions
    , public TSuppressableAccessTrackingOptions
    , public TPrerequisiteOptions
{
    TNullable<std::vector<Stroka>> Attributes;
    TNullable<i64> MaxSize;
};

struct TCreateObjectOptions
    : public TTimeoutOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    std::shared_ptr<const NYTree::IAttributeDictionary> Attributes;
};

struct TCreateNodeOptions
    : public TCreateObjectOptions
    , public TTransactionalOptions
{
    bool Recursive = false;
    bool IgnoreExisting = false;
};

struct TLockNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    bool Waitable = false;
    TNullable<Stroka> ChildKey;
    TNullable<Stroka> AttributeKey;
};

struct TCopyNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    bool Recursive = false;
    bool Force = false;
    bool PreserveAccount = false;
};

struct TMoveNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    bool Recursive = false;
    bool Force = false;
    bool PreserveAccount = true;
};

struct TLinkNodeOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    //! Attributes of a newly created link node.
    std::shared_ptr<const NYTree::IAttributeDictionary> Attributes;
    bool Recursive = false;
    bool IgnoreExisting = false;
};

struct TConcatenateNodesOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
{
    bool Append = false;
};

struct TNodeExistsOptions
    : public TTimeoutOptions
    , public TMasterReadOptions
    , public TTransactionalOptions
    , public TPrerequisiteOptions
{ };

struct TFileReaderOptions
    : public TTransactionalOptions
    , public TSuppressableAccessTrackingOptions
{
    TNullable<i64> Offset;
    TNullable<i64> Length;
    TFileReaderConfigPtr Config;
};

struct TFileWriterOptions
    : public TTransactionalOptions
    , public TPrerequisiteOptions
{
    bool Append = true;
    TFileWriterConfigPtr Config;
};

struct TJournalReaderOptions
    : public TTransactionalOptions
    , public TSuppressableAccessTrackingOptions
{
    TNullable<i64> FirstRowIndex;
    TNullable<i64> RowCount;
    TJournalReaderConfigPtr Config;
};

struct TJournalWriterOptions
    : public TTransactionalOptions
    , public TPrerequisiteOptions
{
    TJournalWriterConfigPtr Config;
    bool EnableMultiplexing = true;
};

struct TTableReaderOptions
    : public TTransactionalOptions
{
    bool Unordered = false;
    NTableClient::TTableReaderConfigPtr Config;
};

struct TStartOperationOptions
    : public TTimeoutOptions
    , public TTransactionalOptions
    , public TMutatingOptions
{ };

struct TAbortOperationOptions
    : public TTimeoutOptions
{
    TNullable<Stroka> AbortMessage;
};

struct TSuspendOperationOptions
    : public TTimeoutOptions
{
    bool AbortRunningJobs = false;
};

struct TResumeOperationOptions
    : public TTimeoutOptions
{ };

struct TCompleteOperationOptions
    : public TTimeoutOptions
{ };

struct TDumpJobContextOptions
    : public TTimeoutOptions
{ };

struct TStraceJobOptions
    : public TTimeoutOptions
{ };

struct TSignalJobOptions
    : public TTimeoutOptions
{ };

struct TAbandonJobOptions
    : public TTimeoutOptions
{ };

struct TPollJobShellOptions
    : public TTimeoutOptions
{ };

struct TAbortJobOptions
    : public TTimeoutOptions
{ };

struct TSelectRowsResult
{
    IRowsetPtr Rowset;
    NQueryClient::TQueryStatistics Statistics;
};

///////////////////////////////////////////////////////////////////////////////

//! Provides a basic set of functions that can be invoked
//! both standalone and inside transaction.
/*
 *  This interface contains methods shared by IClient and ITransaction.
 *
 *  Thread affinity: single
 */
struct IClientBase
    : public virtual TRefCounted
{
    virtual IConnectionPtr GetConnection() = 0;


    // Transactions
    virtual TFuture<ITransactionPtr> StartTransaction(
        NTransactionClient::ETransactionType type,
        const TTransactionStartOptions& options = TTransactionStartOptions()) = 0;


    // Tables
    virtual TFuture<IRowsetPtr> LookupRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TLookupRowsOptions& options = TLookupRowsOptions()) = 0;


    virtual TFuture<TSelectRowsResult> SelectRows(
        const Stroka& query,
        const TSelectRowsOptions& options = TSelectRowsOptions()) = 0;

    // TODO(babenko): batch read and batch write

    // Cypress
    virtual TFuture<NYson::TYsonString> GetNode(
        const NYPath::TYPath& path,
        const TGetNodeOptions& options = TGetNodeOptions()) = 0;

    virtual TFuture<void> SetNode(
        const NYPath::TYPath& path,
        const NYson::TYsonString& value,
        const TSetNodeOptions& options = TSetNodeOptions()) = 0;

    virtual TFuture<void> RemoveNode(
        const NYPath::TYPath& path,
        const TRemoveNodeOptions& options = TRemoveNodeOptions()) = 0;

    virtual TFuture<NYson::TYsonString> ListNode(
        const NYPath::TYPath& path,
        const TListNodeOptions& options = TListNodeOptions()) = 0;

    virtual TFuture<NCypressClient::TNodeId> CreateNode(
        const NYPath::TYPath& path,
        NObjectClient::EObjectType type,
        const TCreateNodeOptions& options = TCreateNodeOptions()) = 0;

    virtual TFuture<NCypressClient::TLockId> LockNode(
        const NYPath::TYPath& path,
        NCypressClient::ELockMode mode,
        const TLockNodeOptions& options = TLockNodeOptions()) = 0;

    virtual TFuture<NCypressClient::TNodeId> CopyNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const TCopyNodeOptions& options = TCopyNodeOptions()) = 0;

    virtual TFuture<NCypressClient::TNodeId> MoveNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const TMoveNodeOptions& options = TMoveNodeOptions()) = 0;

    virtual TFuture<NCypressClient::TNodeId> LinkNode(
        const NYPath::TYPath& srcPath,
        const NYPath::TYPath& dstPath,
        const TLinkNodeOptions& options = TLinkNodeOptions()) = 0;

    virtual TFuture<void> ConcatenateNodes(
        const std::vector<NYPath::TYPath>& srcPaths,
        const NYPath::TYPath& dstPath,
        TConcatenateNodesOptions options = TConcatenateNodesOptions()) = 0;

    virtual TFuture<bool> NodeExists(
        const NYPath::TYPath& path,
        const TNodeExistsOptions& options = TNodeExistsOptions()) = 0;


    // Objects
    virtual TFuture<NObjectClient::TObjectId> CreateObject(
        NObjectClient::EObjectType type,
        const TCreateObjectOptions& options = TCreateObjectOptions()) = 0;


    // Files
    virtual IFileReaderPtr CreateFileReader(
        const NYPath::TYPath& path,
        const TFileReaderOptions& options = TFileReaderOptions()) = 0;

    virtual IFileWriterPtr CreateFileWriter(
        const NYPath::TYPath& path,
        const TFileWriterOptions& options = TFileWriterOptions()) = 0;


    // Journals
    virtual IJournalReaderPtr CreateJournalReader(
        const NYPath::TYPath& path,
        const TJournalReaderOptions& options = TJournalReaderOptions()) = 0;

    virtual IJournalWriterPtr CreateJournalWriter(
        const NYPath::TYPath& path,
        const TJournalWriterOptions& options = TJournalWriterOptions()) = 0;


    // Tables
    virtual TFuture<NTableClient::ISchemalessMultiChunkReaderPtr> CreateTableReader(
        const NYPath::TRichYPath& path,
        const TTableReaderOptions& options = TTableReaderOptions()) = 0;
};

DEFINE_REFCOUNTED_TYPE(IClientBase)

///////////////////////////////////////////////////////////////////////////////

//! A central entry point for all interactions with the YT cluster.
/*!
 *  In contrast to IConnection, each IClient represents an authenticated entity.
 *  The needed username is passed to #IConnection::CreateClient via options.
 *  Note that YT API has no built-in authentication mechanisms so it must be wrapped
 *  with appropriate logic.
 *
 *  Most methods accept |TransactionId| as a part of their options.
 *  A similar effect can be achieved by issuing requests via ITransaction.
 */
struct IClient
    : public virtual IClientBase
{
    //! Terminates all channels.
    //! Aborts all pending uncommitted transactions.
    //! Returns a async flag indicating completion.
    virtual TFuture<void> Terminate() = 0;


    // Transactions
    virtual ITransactionPtr AttachTransaction(
        const NTransactionClient::TTransactionId& transactionId,
        const TTransactionAttachOptions& options = TTransactionAttachOptions()) = 0;


    // Tables
    virtual TFuture<void> MountTable(
        const NYPath::TYPath& path,
        const TMountTableOptions& options = TMountTableOptions()) = 0;

    virtual TFuture<void> UnmountTable(
        const NYPath::TYPath& path,
        const TUnmountTableOptions& options = TUnmountTableOptions()) = 0;

    virtual TFuture<void> RemountTable(
        const NYPath::TYPath& path,
        const TRemountTableOptions& options = TRemountTableOptions()) = 0;

    virtual TFuture<void> FreezeTable(
        const NYPath::TYPath& path,
        const TFreezeTableOptions& options = TFreezeTableOptions()) = 0;

    virtual TFuture<void> UnfreezeTable(
        const NYPath::TYPath& path,
        const TUnfreezeTableOptions& options = TUnfreezeTableOptions()) = 0;

    virtual TFuture<void> ReshardTable(
        const NYPath::TYPath& path,
        const std::vector<NTableClient::TOwningKey>& pivotKeys,
        const TReshardTableOptions& options = TReshardTableOptions()) = 0;

    virtual TFuture<void> ReshardTable(
        const NYPath::TYPath& path,
        int tabletCount,
        const TReshardTableOptions& options = TReshardTableOptions()) = 0;

    virtual TFuture<void> AlterTable(
        const NYPath::TYPath& path,
        const TAlterTableOptions& options = TAlterTableOptions()) = 0;

    virtual TFuture<void> TrimTable(
        const NYPath::TYPath& path,
        int tabletIndex,
        i64 trimmedRowCount,
        const TTrimTableOptions& options = TTrimTableOptions()) = 0;

    virtual TFuture<void> EnableTableReplica(
        const NTabletClient::TTableReplicaId& replicaId,
        const TEnableTableReplicaOptions& options = TEnableTableReplicaOptions()) = 0;

    virtual TFuture<void> DisableTableReplica(
        const NTabletClient::TTableReplicaId& replicaId,
        const TDisableTableReplicaOptions& options = TDisableTableReplicaOptions()) = 0;

    // Security
    virtual TFuture<void> AddMember(
        const Stroka& group,
        const Stroka& member,
        const TAddMemberOptions& options = TAddMemberOptions()) = 0;

    virtual TFuture<void> RemoveMember(
        const Stroka& group,
        const Stroka& member,
        const TRemoveMemberOptions& options = TRemoveMemberOptions()) = 0;

    virtual TFuture<TCheckPermissionResult> CheckPermission(
        const Stroka& user,
        const NYPath::TYPath& path,
        NYTree::EPermission permission,
        const TCheckPermissionOptions& options = TCheckPermissionOptions()) = 0;


    // Scheduler
    virtual TFuture<NScheduler::TOperationId> StartOperation(
        NScheduler::EOperationType type,
        const NYson::TYsonString& spec,
        const TStartOperationOptions& options = TStartOperationOptions()) = 0;

    virtual TFuture<void> AbortOperation(
        const NScheduler::TOperationId& operationId,
        const TAbortOperationOptions& options = TAbortOperationOptions()) = 0;

    virtual TFuture<void> SuspendOperation(
        const NScheduler::TOperationId& operationId,
        const TSuspendOperationOptions& options = TSuspendOperationOptions()) = 0;

    virtual TFuture<void> ResumeOperation(
        const NScheduler::TOperationId& operationId,
        const TResumeOperationOptions& options = TResumeOperationOptions()) = 0;

    virtual TFuture<void> CompleteOperation(
        const NScheduler::TOperationId& operationId,
        const TCompleteOperationOptions& options = TCompleteOperationOptions()) = 0;

    virtual TFuture<void> DumpJobContext(
        const NJobTrackerClient::TJobId& jobId,
        const NYPath::TYPath& path,
        const TDumpJobContextOptions& options = TDumpJobContextOptions()) = 0;

    virtual TFuture<NYson::TYsonString> StraceJob(
        const NJobTrackerClient::TJobId& jobId,
        const TStraceJobOptions& options = TStraceJobOptions()) = 0;

    virtual TFuture<void> SignalJob(
        const NJobTrackerClient::TJobId& jobId,
        const Stroka& signalName,
        const TSignalJobOptions& options = TSignalJobOptions()) = 0;

    virtual TFuture<void> AbandonJob(
        const NJobTrackerClient::TJobId& jobId,
        const TAbandonJobOptions& options = TAbandonJobOptions()) = 0;

    virtual TFuture<NYson::TYsonString> PollJobShell(
        const NJobTrackerClient::TJobId& jobId,
        const NYson::TYsonString& parameters,
        const TPollJobShellOptions& options = TPollJobShellOptions()) = 0;

    virtual TFuture<void> AbortJob(
        const NJobTrackerClient::TJobId& jobId,
        const TAbortJobOptions& options = TAbortJobOptions()) = 0;
};

DEFINE_REFCOUNTED_TYPE(IClient)

///////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

