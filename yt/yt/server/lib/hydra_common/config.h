#pragma once

#include "public.h"

#include <yt/yt/server/lib/io/public.h>

#include <yt/yt/client/api/public.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogConfig
    : public virtual NYTree::TYsonStruct
{
public:
    //! When the number of unflushed data bytes exceeds this value, an automatic data flush is performed.
    i64 DataFlushSize;

    //! When the number of data bytes written since last index flush exceeds this value, an automatic index flush is performed.
    i64 IndexFlushSize;

    //! Interval between consequent automatic flushes.
    TDuration FlushPeriod;

    //! When |false|, no |fdatasync| calls are actually made.
    //! Should only be used in tests and local mode.
    bool EnableSync;

    //! If set, enables preallocating changelog data file to avoid excessive FS metadata
    //! (in particular, file size) updates.
    std::optional<i64> PreallocateSize;

    //! Buffer size for reading the tail of data file during recovery.
    i64 RecoveryBufferSize;

    REGISTER_YSON_STRUCT(TFileChangelogConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogConfig)

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcherConfig
    : public virtual NYTree::TYsonStruct
{
public:
    int IOClass;
    int IOPriority;
    TDuration FlushQuantum;

    REGISTER_YSON_STRUCT(TFileChangelogDispatcherConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogDispatcherConfig)

class TFileChangelogStoreConfig
    : public TFileChangelogConfig
    , public TFileChangelogDispatcherConfig
{
public:
    //! A path where changelogs are stored.
    TString Path;

    //! Maximum number of cached changelogs.
    TSlruCacheConfigPtr ChangelogReaderCache;

    NIO::EIOEngineType IOEngineType;
    NYTree::INodePtr IOConfig;

    REGISTER_YSON_STRUCT(TFileChangelogStoreConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TLocalSnapshotStoreConfig
    : public NYTree::TYsonStruct
{
public:
    //! A path where snapshots are stored.
    TString Path;

    //! Codec used to write snapshots.
    NCompression::ECodec Codec;

    REGISTER_YSON_STRUCT(TLocalSnapshotStoreConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TLocalSnapshotStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TRemoteSnapshotStoreConfig
    : public NYTree::TYsonStruct
{
public:
    NApi::TFileReaderConfigPtr Reader;
    NApi::TFileWriterConfigPtr Writer;

    REGISTER_YSON_STRUCT(TRemoteSnapshotStoreConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRemoteSnapshotStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TRemoteChangelogStoreConfig
    : public NYTree::TYsonStruct
{
public:
    NApi::TJournalReaderConfigPtr Reader;
    NApi::TJournalWriterConfigPtr Writer;
    std::optional<TDuration> LockTransactionTimeout;

    REGISTER_YSON_STRUCT(TRemoteChangelogStoreConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRemoteChangelogStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class THydraJanitorConfig
    : public virtual NYTree::TYsonStruct
{
public:
    std::optional<int> MaxSnapshotCountToKeep;
    std::optional<i64> MaxSnapshotSizeToKeep;
    std::optional<int> MaxChangelogCountToKeep;
    std::optional<i64> MaxChangelogSizeToKeep;

    REGISTER_YSON_STRUCT(THydraJanitorConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(THydraJanitorConfig)

////////////////////////////////////////////////////////////////////////////////

class TLocalHydraJanitorConfig
    : public THydraJanitorConfig
{
public:
    TDuration CleanupPeriod;

    REGISTER_YSON_STRUCT(TLocalHydraJanitorConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TLocalHydraJanitorConfig)

////////////////////////////////////////////////////////////////////////////////

class TDistributedHydraManagerConfig
    : public virtual NYTree::TYsonStruct
{
public:
    //! Timeout for various control RPC requests.
    TDuration ControlRpcTimeout;

    //! The maximum time interval mutations are allowed to occupy the automaton thread
    //! before yielding control to other callbacks.
    TDuration MaxCommitBatchDuration;

    //! Interval between consequent lease lease checks.
    TDuration LeaderLeaseCheckPeriod;

    //! Timeout after which leader lease expires.
    TDuration LeaderLeaseTimeout;

    //! Time a newly elected leader waits before becoming active.
    TDuration LeaderLeaseGraceDelay;

    //! When set to |true|, disables leader grace delay.
    //! For tests only!
    bool DisableLeaderLeaseGraceDelay;

    //! Leader-to-follower commit timeout.
    TDuration CommitFlushRpcTimeout;

    //! Follower-to-leader commit forwarding timeout.
    TDuration CommitForwardingRpcTimeout;

    //! Backoff time for unrecoverable errors causing restart.
    TDuration RestartBackoffTime;

    //! Maximum time allotted to construct a snapshot.
    TDuration SnapshotBuildTimeout;

    //! Maximum time allotted to fork during snapshot building.
    //! If process did not fork within this timeout, it crashes.
    TDuration SnapshotForkTimeout;

    //! Maximum time interval between consequent snapshots.
    TDuration SnapshotBuildPeriod;

    //! Random splay for snapshot building.
    TDuration SnapshotBuildSplay;

    //! Generic timeout for RPC calls during changelog download.
    TDuration ChangelogDownloadRpcTimeout;

    //! Maximum number of bytes to read from a changelog at once.
    i64 MaxChangelogBytesPerRequest;

    //! Maximum number of records to read from a changelog at once.
    int MaxChangelogRecordsPerRequest;

    //! Generic timeout for RPC calls during snapshot download.
    // COMPAT(shakurov): no longer used in Hydra2.
    TDuration SnapshotDownloadRpcTimeout;

    //! Block size used during snapshot download.
    // COMPAT(shakurov): no longer used in Hydra2.
    i64 SnapshotDownloadBlockSize;

    //! Timeout for RPC calls during snapshot download.
    // NB: only used by Hydra2.
    TDuration SnapshotDownloadTotalStreamingTimeout;

    //! Streaming stall timeout for snapshot download.
    // NB: only used by Hydra2.
    TDuration SnapshotDownloadStreamingStallTimeout;

    //! Streaming sliding window size for snapshot download.
    // NB: only used by Hydra2.
    ssize_t SnapshotDownloadWindowSize;

    //! Compression codec for snapshot download.
    // NB: only used by Hydra2.
    NCompression::ECodec SnapshotDownloadStreamingCompressionCodec;

    //! Maximum time to wait before flushing the current batch.
    // COMPAT(babenko): no longer used in Hydra2.
    TDuration MaxCommitBatchDelay;

    //! Maximum number of records to collect before flushing the current batch.
    int MaxCommitBatchRecordCount;

    //! The period between consecutive serializations, i.e. moving
    //! mutations from from draft queue to mutation queue and thus assigning sequence numbers.
    TDuration MutationSerializationPeriod;

    //! The period between consecutive flushes, i.e. sending mutations
    //! from a leader to its followers.
    TDuration MutationFlushPeriod;

    //! Maximum time to wait before syncing with leader.
    TDuration LeaderSyncDelay;

    //! Changelog record count limit.
    /*!
     *  When this limit is reached, the current changelog is rotated and a snapshot
     *  is built.
     */
    int MaxChangelogRecordCount;

    //! Changelog data size limit, in bytes.
    /*!
     *  See #MaxChangelogRecordCount.
     */
    i64 MaxChangelogDataSize;

    //! If true, empty changelogs are preallocated to avoid hiccups of segment rotation.
    // COMPAT(babenko): no longer used in Hydra2
    bool PreallocateChangelogs;

    //! If true, changelogs are gracefully closed on segment rotation and epoch end.
    // COMPAT(babenko): no longer used in Hydra2.
    bool CloseChangelogs;

    //! Interval between automatic "heartbeat" mutations commit.
    /*!
     *  These mutations are no-ops. Committing them regularly helps to ensure
     *  that the quorum is functioning properly.
     */
    TDuration HeartbeatMutationPeriod;

    //! If "heartbeat" mutation commit takes longer than this value, Hydra is restarted.
    TDuration HeartbeatMutationTimeout;

    //! Period for retrying while waiting for changelog record count to become
    //! sufficiently high to proceed with applying mutations.
    TDuration ChangelogRecordCountCheckRetryPeriod;

    //! If mutation logging remains suspended for this period of time,
    //! Hydra restarts.
    TDuration MutationLoggingSuspensionTimeout;

    //! Time to sleep before building a snapshot. Needed for testing.
    TDuration BuildSnapshotDelay;

    //! Persistent stores initialization has exponential retries.
    //! Minimum persistent store initializing backoff time.
    TDuration MinPersistentStoreInitializationBackoffTime;

    //! Maximum persistent store initializing backoff time.
    TDuration MaxPersistentStoreInitializationBackoffTime;

    //! Persistent store initializing backoff time multiplier.
    double PersistentStoreInitializationBackoffTimeMultiplier;

    //! Abandon leader lease request timeout.
    TDuration AbandonLeaderLeaseRequestTimeout;

    //! Enables logging in mutation handlers even during recovery.
    bool ForceMutationLogging;

    //! Enables state hash checker.
    //! It checks that after applying each N-th mutation, automaton state hash is the same on all peers.
    bool EnableStateHashChecker;

    //! Maximum number of entries stored in state hash checker.
    int MaxStateHashCheckerEntryCount;

    //! Followers will report leader every "StateHashCheckerMutationVerificationSamplingRate"-th mutation's state hash.
    int StateHashCheckerMutationVerificationSamplingRate;

    //! In case Hydra leader is not restarted after switch has been initiated within this timeout,
    //! it will restart automatically.
    TDuration LeaderSwitchTimeout;

    //! Maximum number of mutations stored in leader's mutation queue.
    int MaxQueuedMutationCount;

    //! Leader's mutation queue data size limit, in bytes.
    i64 MaxQueuedMutationDataSize;

    //! If set, automaton invariants are checked after each mutation with this probability.
    //! Used for testing purposes only.
    std::optional<double> InvariantsCheckProbability;

    //! Maximum number of in-flight accept mutations request in fast mode.
    int MaxInFlightAcceptMutationsRequestCount;

    //! Maximum number of in-flight mutations in fast mode.
    int MaxInFlightMutationCount;

    //! Maximum in-flight mutations data size in fast mode.
    i64 MaxInFlightMutationDataSize;

    //! If the number of changelogs after last snapshot exceeds this value, force build snapshot
    //! after recovery is complete.
    int MaxChangelogsForRecovery;

    //! If the number of mutations in all changelogs after last snapshot exceeds this value, force build snapshot
    //! after recovery is complete.
    i64 MaxChangelogMutationCountForRecovery;

    //! If data size of all changelogs after last snapshot exceeds this value, force build snapshot
    //! after recovery is complete.
    i64 MaxTotalChangelogSizeForRecovery;

    //! Interval between checkpoint checks.
    TDuration CheckpointCheckPeriod;

    //! Maximum number of changelogs to be created during changelog acquisition if
    //! there is a gap between the last changelog and changelog being acquired.
    int MaxChangelogsToCreateDuringAcquisition;

    //! Alert if no successful snapshots are built.
    bool AlertOnSnapshotFailure;

    REGISTER_YSON_STRUCT(TDistributedHydraManagerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDistributedHydraManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TSerializationDumperConfig
    : public NYTree::TYsonStruct
{
public:
    i64 LowerLimit;
    i64 UpperLimit;

    REGISTER_YSON_STRUCT(TSerializationDumperConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TSerializationDumperConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
