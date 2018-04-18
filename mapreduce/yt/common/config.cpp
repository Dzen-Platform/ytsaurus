#include "config.h"

#include "log.h"
#include "helpers.h"

#include <mapreduce/yt/node/node_builder.h>

#include <library/json/json_reader.h>
#include <library/svnversion/svnversion.h>
#include <library/yson/yson2json_adapter.h>

#include <util/string/strip.h>
#include <util/folder/dirut.h>
#include <util/folder/path.h>
#include <util/stream/file.h>
#include <util/generic/singleton.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/type.h>
#include <util/system/hostname.h>
#include <util/system/user.h>
#include <util/system/env.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

bool TConfig::GetBool(const char* var, bool defaultValue)
{
    TString val = GetEnv(var, "");
    if (val.Empty()) {
        return defaultValue;
    }
    return IsTrue(val);
}

int TConfig::GetInt(const char* var, int defaultValue)
{
    int result = 0;
    TString val = GetEnv(var, "");
    if (val.Empty()) {
        return defaultValue;
    }
    try {
        result = FromString<int>(val);
    } catch (const yexception& e) {
        ythrow yexception() << "Cannot parse " << var << '=' << val << " as integer: " << e.what();
    }
    return result;
}

TDuration TConfig::GetDuration(const char* var, TDuration defaultValue)
{
    return TDuration::Seconds(GetInt(var, defaultValue.Seconds()));
}

EEncoding TConfig::GetEncoding(const char* var)
{
    const TString encodingName = GetEnv(var, "identity");
    EEncoding encoding;
    if (TryFromString(encodingName, encoding)) {
        return encoding;
    } else {
        ythrow yexception() << var << ": encoding '" << encodingName << "' is not supported";
    }
}

void TConfig::ValidateToken(const TString& token)
{
    for (size_t i = 0; i < token.size(); ++i) {
        ui8 ch = token[i];
        if (ch < 0x21 || ch > 0x7e) {
            ythrow yexception() << "Incorrect token character '" << ch << "' at position " << i;
        }
    }
}

TString TConfig::LoadTokenFromFile(const TString& tokenPath)
{
    TFsPath path(tokenPath);
    return path.IsFile() ? Strip(TIFStream(path).ReadAll()) : TString();
}

TNode TConfig::LoadJsonSpec(const TString& strSpec)
{
    TNode spec;
    TStringInput input(strSpec);
    TNodeBuilder builder(&spec);
    TYson2JsonCallbacksAdapter callbacks(&builder);

    Y_ENSURE(NJson::ReadJson(&input, &callbacks), "Cannot parse json spec: " << strSpec);
    Y_ENSURE(spec.IsMap(), "Json spec is not a map");

    return spec;
}

void TConfig::LoadToken()
{
    TString envToken = GetEnv("YT_TOKEN");
    if (envToken) {
        Token = envToken;
    } else {
        TString tokenPath = GetEnv("YT_TOKEN_PATH");
        if (!tokenPath) {
            tokenPath = GetHomeDir() + "/.yt/token";
        }
        Token = LoadTokenFromFile(tokenPath);
    }
    ValidateToken(Token);
}

void TConfig::LoadSpec()
{
    TString strSpec = GetEnv("YT_SPEC", "{}");
    Spec = LoadJsonSpec(strSpec);

    strSpec = GetEnv("YT_TABLE_WRITER", "{}");
    TableWriter = LoadJsonSpec(strSpec);
}

void TConfig::LoadTimings()
{
    ConnectTimeout = GetDuration("YT_CONNECT_TIMEOUT",
        TDuration::Seconds(10));

    SocketTimeout = GetDuration("YT_SOCKET_TIMEOUT",
        GetDuration("YT_SEND_RECEIVE_TIMEOUT", // common
            TDuration::Seconds(60)));

    TxTimeout = GetDuration("YT_TX_TIMEOUT",
        TDuration::Seconds(120));

    PingTimeout = GetDuration("YT_PING_TIMEOUT",
        TDuration::Seconds(5));

    PingInterval = GetDuration("YT_PING_INTERVAL",
        TDuration::Seconds(5));

    WaitLockPollInterval = TDuration::Seconds(5);

    RetryInterval = GetDuration("YT_RETRY_INTERVAL",
        TDuration::Seconds(3));

    ChunkErrorsRetryInterval = GetDuration("YT_CHUNK_ERRORS_RETRY_INTERVAL",
        TDuration::Seconds(60));

    RateLimitExceededRetryInterval = GetDuration("YT_RATE_LIMIT_EXCEEDED_RETRY_INTERVAL",
        TDuration::Seconds(60));

    StartOperationRetryInterval = GetDuration("YT_START_OPERATION_RETRY_INTERVAL",
        TDuration::Seconds(60));
}

TConfig::TConfig()
{
    Hosts = GetEnv("YT_HOSTS", "hosts");
    Pool = GetEnv("YT_POOL");
    Prefix = GetEnv("YT_PREFIX");
    ApiVersion = GetEnv("YT_VERSION", "v3");
    LogLevel = GetEnv("YT_LOG_LEVEL", "error");

    ContentEncoding = GetEncoding("YT_CONTENT_ENCODING");
    AcceptEncoding = GetEncoding("YT_ACCEPT_ENCODING");

    GlobalTxId = GetEnv("YT_TRANSACTION", "");

    ForceIpV4 = GetBool("YT_FORCE_IPV4");
    ForceIpV6 = GetBool("YT_FORCE_IPV6");
    UseHosts = GetBool("YT_USE_HOSTS", true);

    LoadToken();
    LoadSpec();
    LoadTimings();

    RetryCount = Max(GetInt("YT_RETRY_COUNT", 10), 1);
    ReadRetryCount = Max(GetInt("YT_READ_RETRY_COUNT", 30), 1);
    StartOperationRetryCount = Max(GetInt("YT_START_OPERATION_RETRY_COUNT", 30), 1);

    RemoteTempFilesDirectory = GetEnv("YT_FILE_STORAGE",
        "//tmp/yt_wrapper/file_storage");
    RemoteTempTablesDirectory = GetEnv("YT_TEMP_TABLES_STORAGE",
        "//tmp/yt_wrapper/table_storage");

    JobBinary = GetEnv("YT_JOB_BINARY");

    UseClientProtobuf = GetBool("YT_USE_CLIENT_PROTOBUF", false);

    MountSandboxInTmpfs = GetBool("YT_MOUNT_SANDBOX_IN_TMPFS");

    ConnectionPoolSize = GetInt("YT_CONNECTION_POOL_SIZE", 16);
}

TConfig* TConfig::Get()
{
    return Singleton<TConfig>();
}

////////////////////////////////////////////////////////////////////////////////

TProcessState::TProcessState()
{
    try {
        HostName = ::HostName();
    } catch (const yexception& e) {
        ythrow yexception() << "Cannot get host name: " << e.what();
    }

    try {
        UserName = ::GetUsername();
    } catch (const yexception& e) {
        ythrow yexception() << "Cannot get user name: " << e.what();
    }

    Pid = static_cast<int>(getpid());

    if (!ClientVersion) {
        ClientVersion = TStringBuilder() << "YT C++ native r"<< GetProgramSvnRevision();
    }
}

void TProcessState::SetCommandLine(int argc, const char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        CommandLine.push_back(argv[i]);
    }
}

TProcessState* TProcessState::Get()
{
    return Singleton<TProcessState>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

