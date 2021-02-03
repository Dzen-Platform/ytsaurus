#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

class THiveManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent |Ping| requests to remote Hive Manager.
    TDuration PingPeriod;

    //! Interval between consequent idle (i.e. not carrying any payload) |PostMessages|
    //! requests to remote Hive Manager.
    TDuration IdlePostPeriod;

    //! Hive Manager will try to group post requests within this period.
    TDuration PostBatchingPeriod;

    //! Timeout for Ping RPC requests.
    TDuration PingRpcTimeout;

    //! Timeout for Send RPC requests.
    TDuration SendRpcTimeout;

    //! Timeout for Post RPC requests.
    TDuration PostRpcTimeout;

    //! Maximum number of messages to send via a single |PostMessages| request.
    int MaxMessagesPerPost;

    //! Maximum number of bytes to send via a single |PostMessages| request.
    i64 MaxBytesPerPost;

    //! Amount of time TMailbox is allowed to keep a cached channel.
    TDuration CachedChannelTimeout;

    //! Maximum time to wait before syncing with another instance.
    TDuration SyncDelay;

    //! Maximum time to wait before syncing with another instance.
    TDuration SyncTimeout;

    THiveManagerConfig()
    {
        RegisterParameter("ping_period", PingPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("idle_post_period", IdlePostPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("post_batching_period", PostBatchingPeriod)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("ping_rpc_timeout", PingRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("send_rpc_timeout", SendRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("post_rpc_timeout", PostRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("max_messages_per_post", MaxMessagesPerPost)
            .Default(16384);
        RegisterParameter("max_bytes_per_post", MaxBytesPerPost)
            .Default(16_MB);
        RegisterParameter("cached_channel_timeout", CachedChannelTimeout)
            .Default(TDuration::Seconds(3));
        RegisterParameter("sync_delay", SyncDelay)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("sync_timeout", SyncTimeout)
            .Default(TDuration::Seconds(30));
    }
};

DEFINE_REFCOUNTED_TYPE(THiveManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisorConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration ParticipantProbationPeriod;
    TDuration RpcTimeout;
    TDuration ParticipantBackoffTime;

    TTransactionSupervisorConfig()
    {
        RegisterParameter("participant_probation_period", ParticipantProbationPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("rpc_timeout", RpcTimeout)
            .Default(TDuration::Seconds(5));
        RegisterParameter("participant_backoff_time", ParticipantBackoffTime)
            .Default(TDuration::Seconds(5));
    }
};

DEFINE_REFCOUNTED_TYPE(TTransactionSupervisorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent SyncCells requests to the primary Hive Manager.
    TDuration SyncPeriod;

    TCellDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(3));
    }
};

DEFINE_REFCOUNTED_TYPE(TCellDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

class TClusterDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration SyncPeriod;

    //! TTL for GetClusterMeta request.
    TDuration ExpireAfterSuccessfulUpdateTime;
    TDuration ExpireAfterFailedUpdateTime;

    TClusterDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(60));

        RegisterParameter("expire_after_successful_update_time", ExpireAfterSuccessfulUpdateTime)
            .Alias("success_expiration_time")
            .Default(TDuration::Seconds(15));
        RegisterParameter("expire_after_failed_update_time", ExpireAfterFailedUpdateTime)
            .Alias("failure_expiration_time")
            .Default(TDuration::Seconds(15));
    }
};

DEFINE_REFCOUNTED_TYPE(TClusterDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
