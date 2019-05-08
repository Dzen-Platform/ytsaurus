#include <yt/server/lib/core_dump/core_processor_service_proxy.h>
#include <yt/server/lib/core_dump/helpers.h>

#include <yt/ytlib/program/program.h>

#include <yt/core/misc/fs.h>

#include <yt/core/bus/tcp/config.h>
#include <yt/core/bus/tcp/client.h>

#include <yt/core/rpc/bus/channel.h>

#include <util/system/file.h>
#include <util/stream/file.h>

#include <syslog.h>

namespace NYT::NCoreDump {

using namespace NFS;
using namespace NProto;
using namespace NConcurrency;
using namespace NBus;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TCoreForwarderProgram
    : public TProgram
{
public:
    TCoreForwarderProgram()
    {
        Opts_.SetFreeArgsNum(6, 7);
        Opts_.SetFreeArgTitle(0, "PID");
        Opts_.SetFreeArgTitle(1, "UID");
        Opts_.SetFreeArgTitle(2, "EXECUTABLE_NAME");
        Opts_.SetFreeArgTitle(3, "RLIMIT_CORE");
        Opts_.SetFreeArgTitle(4, "JOB_PROXY_SOCKET_DIRECTORY");
        Opts_.SetFreeArgTitle(5, "FALLBACK_PATH");
        Opts_.SetFreeArgTitle(6, "JOB_PROXY_SOCKET");

        // Since syslog is configured to write into stderr, lets make
        // sure that second file descriptor is occupied by open file.
        if (dup(STDERR_FILENO) == -1 && errno == EBADFD) {
            int fd = open("/dev/null", O_WRONLY);
            YCHECK(fd != -1);
            if (fd != STDERR_FILENO) {
                YCHECK(-1 != dup2(fd, STDERR_FILENO));
            }
        }
        openlog("ytserver-core-forwarder", LOG_PID | LOG_PERROR, LOG_USER);
    }

    ~TCoreForwarderProgram()
    {
        closelog();
    }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::CurrentThreadSetName("CoreForwarder");

        ParseFreeArgs(parseResult);

        if (RLimitCore_ == 0) {
            // Do nothing.
            syslog(LOG_INFO, "Doing nothing (RLimitCore: 0)");
            return;
        }

        try {
            TString jobProxySocketNameFile = JobProxySocketNameDirectory_ + "/" + ToString(UserId_);
            if (JobProxySocketPath_ || Exists(jobProxySocketNameFile)) {
                auto jobProxySocketName = JobProxySocketPath_
                    ? *JobProxySocketPath_
                    : TUnbufferedFileInput(jobProxySocketNameFile).ReadLine();
                ForwardCore(jobProxySocketName);
            } else {
                WriteCoreToDisk();
            }
        } catch (const std::exception& ex) {
            OnError(ex.what());
        }
    }

    virtual void OnError(const TString& message) const noexcept override
    {
        syslog(LOG_ERR, "%s", message.c_str());
    }

    void ParseFreeArgs(const NLastGetopt::TOptsParseResult& parseResult)
    {
        auto args = parseResult.GetFreeArgs();

        ProcessId_ = FromString<int>(args[0]);
        UserId_ = FromString<int>(args[1]);
        ExecutableName_ = args[2];
        RLimitCore_ = FromString<ui64>(args[3]);
        JobProxySocketNameDirectory_ = args[4];
        FallbackPath_ = args[5];
        if (args.size() == 7) {
            JobProxySocketPath_ = args[6];
        }

        syslog(LOG_INFO,
            "Processing core dump (Pid: %d, Uid: %d, ExecutableName: %s, RLimitCore: %" PRId64 ", FallbackPath: %s)",
            ProcessId_,
            UserId_,
            ExecutableName_.c_str(),
            RLimitCore_,
            FallbackPath_.c_str());
    }

    void WriteCoreToDisk()
    {
        // We do not fully imitate the system core dump logic here. We only check if
        // core limit is not zero, and then write the whole core dump without truncating
        // it to first RLIMIT_CORE bytes.
        syslog(LOG_INFO, "Writing core to fallback path (FallbackPath: %s)", FallbackPath_.c_str());
        TFile coreFile(FallbackPath_, CreateNew | WrOnly | Seq | CloseOnExec);
        auto size = WriteSparseCoreDump(&Cin, &coreFile);
        coreFile.Close();
        syslog(LOG_INFO, "Finished writing core to disk (Size: %" PRId64 ")", size);
    }

    void ForwardCore(const TString& socketName)
    {
        syslog(LOG_INFO, "Sending core to job proxy (SocketName: %s)", socketName.c_str());

        auto coreProcessorClient = CreateTcpBusClient(TTcpBusClientConfig::CreateUnixDomain(socketName));
        auto coreProcessorChannel = NRpc::NBus::CreateBusChannel(coreProcessorClient);

        TCoreProcessorServiceProxy proxy(coreProcessorChannel);

        TString namedPipePath;

        // Ask job proxy if it needs such a core dump.
        {
            auto req = proxy.StartCoreDump();
            req->set_process_id(ProcessId_);
            req->set_executable_name(ExecutableName_);
            auto rsp = WaitFor(req->Invoke())
                .ValueOrThrow();
            namedPipePath = rsp->named_pipe_path();
        }

        syslog(LOG_INFO, "Writing core to the named pipe (NamedPipePath: %s)", namedPipePath.c_str());

        TUnbufferedFileOutput namedPipeOutput(namedPipePath);
        i64 size = Cin.ReadAll(namedPipeOutput);
        syslog(LOG_INFO, "Finished writing core to the named pipe (Size: %" PRId64 ")", size);
    }

    int ProcessId_ = -1;
    int UserId_ = -1;

    TString ExecutableName_;

    ui64 RLimitCore_ = -1;

    TString JobProxySocketNameDirectory_;
    TString FallbackPath_;
    std::optional<TString> JobProxySocketPath_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCoreDump

int main(int argc, const char** argv)
{
    return NYT::NCoreDump::TCoreForwarderProgram().Run(argc, argv);
}

