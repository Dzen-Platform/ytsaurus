#pragma once

#include "public.h"

#include <yt/client/journal_client/config.h>

#include <yt/client/tablet_client/config.h>

#include <yt/client/chunk_client/config.h>

#include <yt/client/file_client/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EConnectionType,
    (Native)
    (Rpc)
);

////////////////////////////////////////////////////////////////////////////////

class TTableMountCacheConfig
    : public NTabletClient::TTableMountCacheConfig
{
public:
    int OnErrorRetryCount;
    TDuration OnErrorSlackPeriod;

    TTableMountCacheConfig()
    {
        RegisterParameter("on_error_retry_count", OnErrorRetryCount)
            .GreaterThanOrEqual(0)
            .Default(5);
        RegisterParameter("on_error_retry_slack_period", OnErrorSlackPeriod)
            .GreaterThan(TDuration::Zero())
            .Default(TDuration::Seconds(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TTableMountCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TConnectionConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    EConnectionType ConnectionType;
    TTableMountCacheConfigPtr TableMountCache;

    TConnectionConfig()
    {
        RegisterParameter("connection_type", ConnectionType)
            .Default(EConnectionType::Native);
        RegisterParameter("table_mount_cache", TableMountCache)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TConnectionConfig)

////////////////////////////////////////////////////////////////////////////////

class TPersistentQueuePollerConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Try to keep at most this many prefetched rows in memory. This limit is approximate.
    i64 MaxPrefetchRowCount;

    //! Try to keep at most this much prefetched data in memory. This limit is approximate.
    i64 MaxPrefetchDataWeight;

    //! The limit for the number of rows to be requested in a single background fetch request.
    i64 MaxRowsPerFetch;

    //! The limit for the number of rows to be returned by #TPersistentQueuePoller::Poll call.
    i64 MaxRowsPerPoll;

    //! The limit on maximum number of consumed but not yet trimmed row indexes. No new rows are fetched when the limit is reached.
    i64 MaxFetchedUntrimmedRowCount;

    //! When trimming data table, keep the number of consumed but untrimmed rows about this level.
    i64 UntrimmedDataRowsLow;

    //! When more than this many of consumed but untrimmed rows appear in data table, trim the front ones
    //! in accordance to #UntrimmedDataRowsLow.
    i64 UntrimmedDataRowsHigh;

    //! How often the data table is to be polled.
    TDuration DataPollPeriod;

    //! How often the state table is to be trimmed.
    TDuration StateTrimPeriod;

    //! For how long to backoff when a state conflict is detected.
    TDuration BackoffTime;

    TPersistentQueuePollerConfig()
    {
        RegisterParameter("max_prefetch_row_count", MaxPrefetchRowCount)
            .GreaterThan(0)
            .Default(1024);
        RegisterParameter("max_prefetch_data_weight", MaxPrefetchDataWeight)
            .GreaterThan(0)
            .Default((i64) 16 * 1024 * 1024);
        RegisterParameter("max_rows_per_fetch", MaxRowsPerFetch)
            .GreaterThan(0)
            .Default(512);
        RegisterParameter("max_rows_per_poll", MaxRowsPerPoll)
            .GreaterThan(0)
            .Default(1);
        RegisterParameter("max_fetched_untrimmed_row_count", MaxFetchedUntrimmedRowCount)
            .GreaterThan(0)
            .Default(40000);
        RegisterParameter("untrimmed_data_rows_low", UntrimmedDataRowsLow)
            .Default(0);
        RegisterParameter("untrimmed_data_rows_high", UntrimmedDataRowsHigh)
            .Default(std::numeric_limits<i64>::max());
        RegisterParameter("data_poll_period", DataPollPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("state_trim_period", StateTrimPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("backoff_time", BackoffTime)
            .Default(TDuration::Seconds(5));

        RegisterPostprocessor([&] {
            if (UntrimmedDataRowsLow > UntrimmedDataRowsHigh) {
                THROW_ERROR_EXCEPTION("\"untrimmed_data_rows_low\" must not exceed \"untrimmed_data_rows_high\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TPersistentQueuePollerConfig)

////////////////////////////////////////////////////////////////////////////////

class TFileReaderConfig
    : public virtual NChunkClient::TMultiChunkReaderConfig
{ };

DEFINE_REFCOUNTED_TYPE(TFileReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TFileWriterConfig
    : public NChunkClient::TMultiChunkWriterConfig
    , public NFileClient::TFileChunkWriterConfig
{ };

DEFINE_REFCOUNTED_TYPE(TFileWriterConfig)

////////////////////////////////////////////////////////////////////////////////

class TJournalReaderConfig
    : public NJournalClient::TChunkReaderConfig
    , public TWorkloadConfig
{ };

DEFINE_REFCOUNTED_TYPE(TJournalReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TJournalWriterConfig
    : public virtual TWorkloadConfig
{
public:
    TDuration MaxBatchDelay;
    i64 MaxBatchDataSize;
    int MaxBatchRowCount;

    int MaxFlushRowCount;
    i64 MaxFlushDataSize;

    bool PreferLocalHost;

    TDuration NodeRpcTimeout;
    TDuration NodePingPeriod;
    TDuration NodeBanTimeout;

    int MaxChunkRowCount;
    i64 MaxChunkDataSize;
    TDuration MaxChunkSessionDuration;

    NRpc::TRetryingChannelConfigPtr NodeChannel;

    TDuration PrerequisiteTransactionProbePeriod;

    bool IgnoreClosing; // for testing purposes only

    TJournalWriterConfig()
    {
        RegisterParameter("max_batch_delay", MaxBatchDelay)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("max_batch_data_size", MaxBatchDataSize)
            .Default(16_MB);
        RegisterParameter("max_batch_row_count", MaxBatchRowCount)
            .Default(100'000);

        RegisterParameter("max_flush_row_count", MaxFlushRowCount)
            .Default(100'000);
        RegisterParameter("max_flush_data_size", MaxFlushDataSize)
            .Default(100_MB);

        RegisterParameter("prefer_local_host", PreferLocalHost)
            .Default(true);

        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("node_ping_period", NodePingPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("node_ban_timeout", NodeBanTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("max_chunk_row_count", MaxChunkRowCount)
            .GreaterThan(0)
            .Default(1'000'000);
        RegisterParameter("max_chunk_data_size", MaxChunkDataSize)
            .GreaterThan(0)
            .Default(10_GB);
        RegisterParameter("max_chunk_session_duration", MaxChunkSessionDuration)
            .Default(TDuration::Hours(60));

        RegisterParameter("node_channel", NodeChannel)
            .DefaultNew();

        RegisterParameter("prerequisite_transaction_probe_period", PrerequisiteTransactionProbePeriod)
            .Default(TDuration::Seconds(60));

        RegisterParameter("ignore_closing", IgnoreClosing)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TJournalWriterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

