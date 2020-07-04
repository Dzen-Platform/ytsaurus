#pragma once

#include <yt/client/table_client/public.h>

#include <yt/client/transaction_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

// Keep in sync with NRpcProxy::NProto::EMasterReadKind.
// On cache miss request is redirected to next level cache:
// Local cache -> (node) cache -> master cache
DEFINE_ENUM(EMasterChannelKind,
    ((Leader)                (0))
    ((Follower)              (1))
    // Use local (per-connection) cache.
    ((LocalCache)            (4))
    // Use cache located on nodes.
    ((Cache)                 (2))
    // Use cache located on masters (if caching on masters is enabled).
    ((MasterCache)           (3))
);

DEFINE_ENUM(EUserWorkloadCategory,
    (Batch)
    (Interactive)
    (Realtime)
);

DEFINE_ENUM(EErrorCode,
    ((TooManyConcurrentRequests)                         (1900))
    ((JobArchiveUnavailable)                             (1910))
    ((OperationProgressOutdated)                         (1911))
    ((NoSuchOperation)                                   (1915))
    ((NoSuchJob)                                         (1916))
    ((NoSuchAttribute)                                   (1920))
);

DEFINE_ENUM(ERowModificationType,
    ((Write)            (0))
    ((Delete)           (1))
    ((VersionedWrite)   (2))
    ((ReadLockWrite)    (3))
);

DEFINE_ENUM(ETransactionCoordinatorCommitMode,
    // Success is reported when phase 2 starts (all participants have prepared but not yet committed).
    ((Eager)  (0))
    // Success is reported when transaction is finished (all participants have committed).
    ((Lazy)   (1))
);

////////////////////////////////////////////////////////////////////////////////

template <class TRow>
struct IRowset;
template <class TRow>
using IRowsetPtr = TIntrusivePtr<IRowset<TRow>>;

using IUnversionedRowset = IRowset<NTableClient::TUnversionedRow>;
using IVersionedRowset = IRowset<NTableClient::TVersionedRow>;

DECLARE_REFCOUNTED_TYPE(IUnversionedRowset)
DECLARE_REFCOUNTED_TYPE(IVersionedRowset)
DECLARE_REFCOUNTED_STRUCT(IPersistentQueueRowset)
DECLARE_REFCOUNTED_STRUCT(TSkynetSharePartsLocations);

struct TAdminOptions;
struct TClientOptions;
struct TTransactionParticipantOptions;

struct TTimeoutOptions;
struct TTransactionalOptions;
struct TPrerequisiteOptions;
struct TMasterReadOptions;
struct TMutatingOptions;
struct TSuppressableAccessTrackingOptions;
struct TTabletRangeOptions;

struct TGetFileFromCacheResult;
struct TPutFileToCacheResult;

DECLARE_REFCOUNTED_STRUCT(IConnection)
DECLARE_REFCOUNTED_STRUCT(IAdmin)
DECLARE_REFCOUNTED_STRUCT(IClientBase)
DECLARE_REFCOUNTED_STRUCT(IClient)
DECLARE_REFCOUNTED_STRUCT(ITransaction)
DECLARE_REFCOUNTED_STRUCT(IStickyTransactionPool)

DECLARE_REFCOUNTED_STRUCT(ITableReader)
DECLARE_REFCOUNTED_STRUCT(ITableWriter)

DECLARE_REFCOUNTED_STRUCT(IFileReader)
DECLARE_REFCOUNTED_STRUCT(IFileWriter)

DECLARE_REFCOUNTED_STRUCT(IJournalReader)
DECLARE_REFCOUNTED_STRUCT(IJournalWriter)

DECLARE_REFCOUNTED_CLASS(TPersistentQueuePoller)

DECLARE_REFCOUNTED_CLASS(TTableMountCacheConfig)
DECLARE_REFCOUNTED_CLASS(TConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TPersistentQueuePollerConfig)

DECLARE_REFCOUNTED_CLASS(TFileReaderConfig)
DECLARE_REFCOUNTED_CLASS(TFileWriterConfig)
DECLARE_REFCOUNTED_CLASS(TJournalReaderConfig)
DECLARE_REFCOUNTED_CLASS(TJournalWriterConfig)

DECLARE_REFCOUNTED_STRUCT(TPrerequisiteRevisionConfig)

DECLARE_REFCOUNTED_STRUCT(TSchedulingOptions)

DECLARE_REFCOUNTED_CLASS(TJobInputReader)

DECLARE_REFCOUNTED_CLASS(TClientCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

