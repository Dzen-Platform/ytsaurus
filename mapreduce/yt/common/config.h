#pragma once

#include <mapreduce/yt/interface/node.h>
#include <mapreduce/yt/interface/common.h>

#include <util/generic/maybe.h>
#include <util/generic/string.h>
#include <util/datetime/base.h>

namespace NYT {

enum EEncoding : int
{
    E_IDENTITY  /* "identity" */,
    E_GZIP      /* "gzip" */,
    E_BROTLI    /* "br" */,
    E_Z_LZ4     /* "z-lz4" */,
};

enum class ENodeReaderFormat : int
{
    Yson,  // Always use YSON format,
    Skiff, // Always use Skiff format, throw exception if it's not possible (non-strict schema, dynamic table etc.)
    Auto,  // Use Skiff format if it's possible, YSON otherwise
};

enum class ETraceHttpRequestsMode
{
    // Never dump http requests.
    Never /* "never" */,
    // Dump failed http requests.
    Error /* "error" */,
    // Dump all http requests.
    Always /* "always" */,
};

////////////////////////////////////////////////////////////////////////////////

struct TConfig
{
    TString Hosts;
    TString Pool;
    TString Token;
    TString Prefix;
    TString ApiVersion;
    TString LogLevel;

    // Compression for data that is sent to YT cluster.
    EEncoding ContentEncoding;

    // Compression for data that is read from YT cluster.
    EEncoding AcceptEncoding;

    TString GlobalTxId;

    bool ForceIpV4;
    bool ForceIpV6;
    bool UseHosts;

    TNode Spec;
    TNode TableWriter;

    TDuration ConnectTimeout;
    TDuration SocketTimeout;
    TDuration AddressCacheExpirationTimeout;
    TDuration TxTimeout;
    TDuration PingTimeout;
    TDuration PingInterval;

    // How often should we poll for lock state
    TDuration WaitLockPollInterval;

    TDuration RetryInterval;
    TDuration ChunkErrorsRetryInterval;

    TDuration RateLimitExceededRetryInterval;
    TDuration StartOperationRetryInterval;

    int RetryCount;
    int ReadRetryCount;
    int StartOperationRetryCount;

    TString RemoteTempFilesDirectory;
    TString RemoteTempTablesDirectory;

    //
    // Infer schemas for nonexstent tables from typed rows (e.g. protobuf)
    // when writing from operation or client writer.
    // This options can be overriden in TOperationOptions and TTableWriterOptions.
    bool InferTableSchema;

    bool UseClientProtobuf;
    ENodeReaderFormat NodeReaderFormat;
    bool ProtobufFormatWithDescriptors;

    int ConnectionPoolSize;

    /// Defines replication factor that is used for files that are uploaded to YT
    /// to use them in operations.
    int FileCacheReplicationFactor = 10;

    bool MountSandboxInTmpfs;

    TRichYPath ApiFilePathOptions;

    // Testing options, should never be used in user programs.
    bool UseAbortableResponse = false;
    bool EnableDebugMetrics = false;

    //
    // There is optimization used with local YT that enables to skip binary upload and use real binary path.
    // When EnableLocalModeOptimization is set to false this optimization is completely disabled.
    bool EnableLocalModeOptimization = true;

    //
    // If you want see stderr even if you jobs not failed set this true.
    bool WriteStderrSuccessfulJobs = false;

    //
    // This configuration is useful for debug.
    // If set to ETraceHttpRequestsMode::Error library will dump all http error requests.
    // If set to ETraceHttpRequestsMode::All library will dump all http requests.
    // All tracing occurres as DEBUG level logging.
    ETraceHttpRequestsMode TraceHttpRequestsMode = ETraceHttpRequestsMode::Never;

    TString SkynetApiHost;

    // Sets SO_PRIORITY option on the socket
    TMaybe<int> SocketPriority;

    static bool GetBool(const char* var, bool defaultValue = false);
    static int GetInt(const char* var, int defaultValue);
    static TDuration GetDuration(const char* var, TDuration defaultValue);
    static EEncoding GetEncoding(const char* var);

    static void ValidateToken(const TString& token);
    static TString LoadTokenFromFile(const TString& tokenPath);

    static TNode LoadJsonSpec(const TString& strSpec);

    static TRichYPath LoadApiFilePathOptions(const TString& ysonMap);

    void LoadToken();
    void LoadSpec();
    void LoadTimings();

    void Reset();

    TConfig();

    static TConfig* Get();
};

////////////////////////////////////////////////////////////////////////////////

struct TProcessState
{
    TString FqdnHostName;
    TString UserName;
    TVector<TString> CommandLine;

    // Command line with everything that looks like tokens censored.
    TVector<TString> CensoredCommandLine;
    int Pid;
    TString ClientVersion;

    TProcessState();

    void SetCommandLine(int argc, const char* argv[]);

    static TProcessState* Get();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

