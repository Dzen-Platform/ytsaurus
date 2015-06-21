#pragma once

#include "public.h"

#include <core/compression/public.h>

#include <core/erasure/public.h>

#include <core/misc/error.h>
#include <core/misc/config.h>

#include <core/rpc/config.h>

#include <core/ytree/yson_serializable.h>

#include <ytlib/node_tracker_client/public.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

class TReplicationReaderConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Timeout for a block request.
    TDuration BlockRpcTimeout;

    //! Timeout for a meta request.
    TDuration MetaRpcTimeout;

    //! Time to wait before asking the master for seeds.
    TDuration RetryBackoffTime;

    //! Maximum number of attempts to fetch new seeds.
    int RetryCount;

    //! Time to wait before making another pass with same seeds.
    //! Increases exponentially with every pass, from MinPassBackoffTime to MaxPassBackoffTime.
    TDuration MinPassBackoffTime;
    TDuration MaxPassBackoffTime;
    double PassBackoffTimeMultiplier;

    //! Maximum number of passes with same seeds.
    int PassCount;

    //! Enable fetching blocks from peers suggested by seeds.
    bool FetchFromPeers;

    //! Timeout after which a node forgets about the peer.
    //! Only makes sense if the reader is equipped with peer descriptor.
    TDuration PeerExpirationTimeout;

    //! If |true| then fetched blocks are cached by the node.
    bool PopulateCache;

    //! If |true| then the master may be asked for seeds.
    bool AllowFetchingSeedsFromMaster;

    TReplicationReaderConfig()
    {
        RegisterParameter("block_rpc_timeout", BlockRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("meta_rpc_timeout", MetaRpcTimeout)
            .Default(TDuration::Seconds(30));
        RegisterParameter("retry_backoff_time", RetryBackoffTime)
            .Default(TDuration::Seconds(3));
        RegisterParameter("retry_count", RetryCount)
            .Default(20);
        RegisterParameter("min_pass_backoff_time", MinPassBackoffTime)
            .Default(TDuration::Seconds(3));
        RegisterParameter("max_pass_backoff_time", MaxPassBackoffTime)
            .Default(TDuration::Seconds(60));
        RegisterParameter("pass_backoff_time_multiplier", PassBackoffTimeMultiplier)
            .GreaterThan(1)
            .Default(1.5);
        RegisterParameter("pass_count", PassCount)
            .Default(500);
        RegisterParameter("fetch_from_peers", FetchFromPeers)
            .Default(true);
        RegisterParameter("peer_expiration_timeout", PeerExpirationTimeout)
            .Default(TDuration::Seconds(300));
        RegisterParameter("populate_cache", PopulateCache)
            .Default(true);
        RegisterParameter("allow_fetching_seeds_from_master", AllowFetchingSeedsFromMaster)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TReplicationReaderConfig)

///////////////////////////////////////////////////////////////////////////////

class TRemoteReaderOptions
    : public virtual NYTree::TYsonSerializable
{
public:
    Stroka NetworkName;
    EReadSessionType SessionType;

    TRemoteReaderOptions()
    {
        RegisterParameter("network_name", NetworkName)
            .Default(NNodeTrackerClient::InterconnectNetworkName);
        RegisterParameter("session_type", SessionType)
            .Default(EReadSessionType::User);
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteReaderOptions)

///////////////////////////////////////////////////////////////////////////////

class TSequentialReaderConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Prefetch window size (in bytes).
    i64 WindowSize;

    //! Maximum amount of data to be transfered via a single RPC request.
    i64 GroupSize;

    TNullable<double> SamplingRate;
    TNullable<ui64> SamplingSeed;

    TSequentialReaderConfig()
    {
        RegisterParameter("window_size", WindowSize)
            .Default((i64) 20 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("group_size", GroupSize)
            .Default((i64) 15 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("sampling_rate", SamplingRate)
            .Default()
            .InRange(0, 1);
        RegisterParameter("sampling_seed", SamplingSeed)
            .Default();

        RegisterValidator([&] () {
            if (GroupSize > WindowSize) {
                THROW_ERROR_EXCEPTION("\"group_size\" cannot be larger than \"window_size\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TSequentialReaderConfig)

///////////////////////////////////////////////////////////////////////////////

class TReplicationWriterConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Maximum window size (in bytes).
    i64 SendWindowSize;

    //! Maximum group size (in bytes).
    i64 GroupSize;

    //! RPC requests timeout.
    /*!
     *  This timeout is especially useful for |PutBlocks| calls to ensure that
     *  uploading is not stalled.
     */
    TDuration NodeRpcTimeout;

    int UploadReplicationFactor;

    int MinUploadReplicationFactor;

    bool PreferLocalHost;

    //! Interval between consecutive pings to Data Nodes.
    TDuration NodePingPeriod;

    //! If |true| then written blocks are cached by the node.
    bool PopulateCache;

    bool SyncOnClose;

    TReplicationWriterConfig()
    {
        RegisterParameter("send_window_size", SendWindowSize)
            .Default((i64) 32 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("group_size", GroupSize)
            .Default((i64) 10 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("upload_replication_factor", UploadReplicationFactor)
            .GreaterThanOrEqual(1)
            .Default(2);
        RegisterParameter("min_upload_replication_factor", MinUploadReplicationFactor)
            .Default(2)
            .GreaterThanOrEqual(1);
        RegisterParameter("prefer_local_host", PreferLocalHost)
            .Default(true);
        RegisterParameter("node_ping_interval", NodePingPeriod)
            .Default(TDuration::Seconds(10));
        RegisterParameter("populate_cache", PopulateCache)
            .Default(false);
        RegisterParameter("sync_on_close", SyncOnClose)
            .Default(true);

        RegisterValidator([&] () {
            if (SendWindowSize < GroupSize) {
                THROW_ERROR_EXCEPTION("\"send_window_size\" cannot be less than \"group_size\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TReplicationWriterConfig)

///////////////////////////////////////////////////////////////////////////////

class TRemoteWriterOptions
    : public virtual NYTree::TYsonSerializable
{
public:
    Stroka NetworkName;
    EWriteSessionType SessionType;

    TRemoteWriterOptions()
    {
        RegisterParameter("network_name", NetworkName)
            .Default(NNodeTrackerClient::InterconnectNetworkName);
        RegisterParameter("session_type", SessionType)
            .Default(EWriteSessionType::User);
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteWriterOptions)

///////////////////////////////////////////////////////////////////////////////

class TErasureWriterConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    i64 ErasureWindowSize;

    TErasureWriterConfig()
    {
        RegisterParameter("erasure_window_size", ErasureWindowSize)
            .Default((i64) 8 * 1024 * 1024)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TErasureWriterConfig)

///////////////////////////////////////////////////////////////////////////////

class TEncodingWriterConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    i64 EncodeWindowSize;
    double DefaultCompressionRatio;
    bool VerifyCompression;

    TEncodingWriterConfig()
    {
        RegisterParameter("encode_window_size", EncodeWindowSize)
            .Default((i64) 16 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("default_compression_ratio", DefaultCompressionRatio)
            .Default(0.2);
        RegisterParameter("verify_compression", VerifyCompression)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TEncodingWriterConfig)

///////////////////////////////////////////////////////////////////////////////

class TEncodingWriterOptions
    : public virtual NYTree::TYsonSerializable
{
public:
    NCompression::ECodec CompressionCodec;
    bool ChunksEden;

    TEncodingWriterOptions()
    {
        RegisterParameter("compression_codec", CompressionCodec)
            .Default(NCompression::ECodec::None);
        RegisterParameter("chunks_eden", ChunksEden)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TEncodingWriterOptions)

///////////////////////////////////////////////////////////////////////////////

class TDispatcherConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    int CompressionPoolSize;
    int ErasurePoolSize;

    TDispatcherConfig()
    {
        RegisterParameter("compression_pool_size", CompressionPoolSize)
            .Default(4)
            .GreaterThan(0);
        RegisterParameter("erasure_pool_size", ErasurePoolSize)
            .Default(4)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TDispatcherConfig)

///////////////////////////////////////////////////////////////////////////////

class TMultiChunkWriterConfig
    : public TReplicationWriterConfig
    , public TErasureWriterConfig
{
public:
    i64 DesiredChunkSize;
    i64 MaxMetaSize;

    bool ChunksMovable;

    bool SyncChunkSwitch;

    NErasure::ECodec ErasureCodec;

    TMultiChunkWriterConfig()
    {
        RegisterParameter("desired_chunk_size", DesiredChunkSize)
            .GreaterThan(0)
            .Default((i64) 1024 * 1024 * 1024);
        RegisterParameter("max_meta_size", MaxMetaSize)
            .GreaterThan(0)
            .LessThanOrEqual((i64) 64 * 1024 * 1024)
            .Default((i64) 30 * 1024 * 1024);
        RegisterParameter("chunks_movable", ChunksMovable)
            .Default(true);
        RegisterParameter("sync_chunk_switch", SyncChunkSwitch)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkWriterConfig)

///////////////////////////////////////////////////////////////////////////////

class TMultiChunkWriterOptions
    : public virtual TEncodingWriterOptions
    , public virtual TRemoteWriterOptions
{
public:
    int ReplicationFactor;
    Stroka Account;
    bool ChunksVital;

    NErasure::ECodec ErasureCodec;

    TMultiChunkWriterOptions()
    {
        RegisterParameter("replication_factor", ReplicationFactor)
            .GreaterThanOrEqual(1)
            .Default(3);
        RegisterParameter("account", Account)
            .NonEmpty();
        RegisterParameter("chunks_vital", ChunksVital)
            .Default(true);
        RegisterParameter("erasure_codec", ErasureCodec)
            .Default(NErasure::ECodec::None);
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkWriterOptions)

///////////////////////////////////////////////////////////////////////////////

class TMultiChunkReaderConfig
    : public virtual TReplicationReaderConfig
    , public virtual TSequentialReaderConfig
{
public:
    i64 MaxBufferSize;


    TMultiChunkReaderConfig()
    {
        RegisterParameter("max_buffer_size", MaxBufferSize)
            .GreaterThan(0L)
            .LessThanOrEqual((i64) 10 * 1024 * 1024 * 1024)
            .Default((i64) 100 * 1024 * 1024);

        RegisterValidator([&] () {
            if (MaxBufferSize < 2 * WindowSize) {
                THROW_ERROR_EXCEPTION("\"max_buffer_size\" cannot be less than twice \"window_size\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkReaderConfig)

///////////////////////////////////////////////////////////////////////////////

class TMultiChunkReaderOptions
    : public TRemoteReaderOptions
{
public:
    bool KeepInMemory;

    TMultiChunkReaderOptions()
    {
        RegisterParameter("keep_in_memory", KeepInMemory)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkReaderOptions)

///////////////////////////////////////////////////////////////////////////////

class TFetcherConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    NRpc::TRetryingChannelConfigPtr NodeChannel;

    TDuration NodeRpcTimeout;

    TFetcherConfig()
    {
        RegisterParameter("node_channel", NodeChannel)
            .DefaultNew();
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(30));
    }
};

DEFINE_REFCOUNTED_TYPE(TFetcherConfig)

///////////////////////////////////////////////////////////////////////////////

class TBlockCacheConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TSlruCacheConfigPtr CompressedData;
    TSlruCacheConfigPtr UncompressedData;

    i64 GetTotalCapacity() const
    {
        return
            CompressedData->Capacity +
            UncompressedData->Capacity;
    }

    TBlockCacheConfig()
    {
        RegisterParameter("compressed_data", CompressedData)
            .DefaultNew();
        RegisterParameter("uncompressed_data", UncompressedData)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TBlockCacheConfig)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
