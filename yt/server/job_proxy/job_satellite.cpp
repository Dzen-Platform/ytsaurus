#include "job_satellite.h"
#include "job_prober_service.h"
#include "job_satellite_connection.h"
#include "user_job.h"
#include "user_job_synchronizer.h"
#include "private.h"

#include <yt/server/lib/exec_agent/public.h>

#include <yt/server/lib/shell/shell_manager.h>

#include <yt/core/bus/tcp/config.h>
#include <yt/core/bus/tcp/server.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/logging/config.h>
#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/fs.h>
#include <yt/library/process/process.h>

#include <yt/core/rpc/bus/server.h>
#include <yt/core/rpc/server.h>

#include <yt/ytlib/tools/tools.h>
#include <yt/ytlib/tools/stracer.h>
#include <yt/ytlib/tools/signaler.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/yson/string.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/proc.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/client/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/core/misc/shutdown.h>

#include <util/generic/guid.h>
#include <util/system/fs.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

namespace NYT::NJobProxy {

using namespace NRpc;
using namespace NYT::NBus;
using namespace NConcurrency;
using namespace NShell;
using namespace NTools;

using NChunkClient::TChunkId;
using NYTree::INodePtr;
using NJobTrackerClient::TJobId;
using NYson::TYsonString;
using NJobProberClient::IJobProbe;
using NExecAgent::EJobEnvironmentType;

static const NLogging::TLogger JobSatelliteLogger("JobSatellite");
static const auto& Logger = JobSatelliteLogger;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TJobProbeTools)

////////////////////////////////////////////////////////////////////////////////

struct IPidsHolder
{
    virtual ~IPidsHolder() = default;
    virtual std::vector<int> GetPids() = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TFreezerPidsHolder
    : public IPidsHolder
{
public:
    explicit TFreezerPidsHolder(const TString& name)
        : Freezer_(NCGroup::TFreezer::Name, name)
    { }

    virtual std::vector<int> GetPids() override
    {
        return Freezer_.GetProcesses();
    }

private:
    NCGroup::TNonOwningCGroup Freezer_;
};

////////////////////////////////////////////////////////////////////////////////

class TSimplePidsHolder
    : public IPidsHolder
{
public:
    explicit TSimplePidsHolder(int uid)
        : Uid_(uid)
    { }

    virtual std::vector<int> GetPids() override
    {
        return GetPidsByUid(Uid_);
    }

private:
    const int Uid_;
};

////////////////////////////////////////////////////////////////////////////////

class TContainerPidsHolder
    : public IPidsHolder
{
public:
    explicit TContainerPidsHolder(int uid)
        : Uid_(uid)
    { }

    virtual std::vector<int> GetPids() override
    {
        auto pids = GetPidsByUid(Uid_);
        auto myPid = ::getpid();
        auto it = std::find(pids.begin(), pids.end(), myPid);
        if (it != pids.end()) {
            pids.erase(it);
        }
        return pids;
    }

private:
    const int Uid_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobProbeTools
    : public TRefCounted
{
public:
    ~TJobProbeTools();
    static TJobProbeToolsPtr Create(
        TJobId jobId,
        pid_t rootPid,
        int uid,
        const std::vector<TString>& env,
        EJobEnvironmentType environmentType,
        bool enableSecureVaultVariablesInJobShell);

    TYsonString StraceJob();
    void SignalJob(const TString& signalName);
    TYsonString PollJobShell(const TYsonString& parameters);
    TFuture<void> AsyncGracefulShutdown(const TError& error);

private:
    const EJobEnvironmentType EnvironmentType_;
    const bool EnableSecureVaultVariablesInJobShell_;
    const pid_t RootPid_;
    const int Uid_;
    const std::vector<TString> Environment_;
    const NConcurrency::TActionQueuePtr AuxQueue_;

    std::unique_ptr<IPidsHolder> PidsHolder_;

    std::atomic_flag Stracing_ = ATOMIC_FLAG_INIT;
    IShellManagerPtr ShellManager_;

    TJobProbeTools(pid_t rootPid,
        int uid,
        const std::vector<TString>& env,
        EJobEnvironmentType environmentType,
        bool enableSecureVaultVariablesInJobShell);
    void Init(TJobId jobId);

    DECLARE_NEW_FRIEND();
};

DEFINE_REFCOUNTED_TYPE(TJobProbeTools)

////////////////////////////////////////////////////////////////////////////////

TJobProbeTools::TJobProbeTools(
    pid_t rootPid,
    int uid,
    const std::vector<TString>& env,
    EJobEnvironmentType environmentType,
    bool enableSecureVaultVariablesInJobShell)
    : EnvironmentType_(environmentType)
    , EnableSecureVaultVariablesInJobShell_(enableSecureVaultVariablesInJobShell)
    , RootPid_(rootPid)
    , Uid_(uid)
    , Environment_(env)
    , AuxQueue_(New<TActionQueue>("JobAux"))
{ }

TJobProbeToolsPtr TJobProbeTools::Create(
    TJobId jobId,
    pid_t rootPid,
    int uid,
    const std::vector<TString>& env,
    EJobEnvironmentType environmentType,
    bool enableSecureVaultVariablesInJobShell)
{
    auto tools = New<TJobProbeTools>(rootPid, uid, env, environmentType, enableSecureVaultVariablesInJobShell);
    try {
        tools->Init(jobId);
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Unable to create cgroup tools");
        THROW_ERROR_EXCEPTION("Unable to create cgroup tools")
            << ex;
    }
    return tools;
}

void TJobProbeTools::Init(TJobId jobId)
{
    switch (EnvironmentType_) {
        case EJobEnvironmentType::Cgroups:
            PidsHolder_.reset(new TFreezerPidsHolder("user_job_" + ToString(jobId)));
            break;

        case EJobEnvironmentType::Porto:
            PidsHolder_.reset(new TContainerPidsHolder(Uid_));
            break;

        case EJobEnvironmentType::Simple:
            PidsHolder_.reset(new TSimplePidsHolder(Uid_));
            break;

        default:
            YT_ABORT();
    }

    auto currentWorkDir = NFs::CurrentWorkingDirectory();
    currentWorkDir = currentWorkDir.substr(0, currentWorkDir.find_last_of("/"));

    // Copy environment to process arguments
    std::vector<TString> shellEnvironment;
    shellEnvironment.reserve(Environment_.size());

    std::vector<TString> visibleEnvironment;
    visibleEnvironment.reserve(Environment_.size());

    for (const auto& var : Environment_) {
        bool allowSecureVaultVariable = (EnableSecureVaultVariablesInJobShell_ || !var.StartsWith("YT_SECURE_VAULT_"));
        if (var.StartsWith("YT_") && allowSecureVaultVariable) {
            shellEnvironment.emplace_back(var);
        }
        if (allowSecureVaultVariable) {
            visibleEnvironment.emplace_back(var);
        }
    }

    ShellManager_ = CreateShellManager(
        NFS::CombinePaths(currentWorkDir, NExecAgent::SandboxDirectoryNames[NExecAgent::ESandboxKind::Home]),
        Uid_,
        EnvironmentType_ == EJobEnvironmentType::Cgroups ? std::optional<TString>("user_job_" + ToString(jobId)) : std::optional<TString>(),
        Format("Job environment:\n%v\n", JoinToString(visibleEnvironment, AsStringBuf("\n"))),
        std::move(shellEnvironment));
}

TJobProbeTools::~TJobProbeTools()
{
    if (ShellManager_) {
        BIND(&IShellManager::Terminate, ShellManager_, TError())
            .Via(AuxQueue_->GetInvoker())
            .Run();
    }
}

TYsonString TJobProbeTools::StraceJob()
{
    if (Stracing_.test_and_set()) {
        THROW_ERROR_EXCEPTION("Another strace session is in progress");
    }

    auto guard = Finally([&] () {
        Stracing_.clear();
    });

    auto pids = PidsHolder_->GetPids();

    auto it = std::find(pids.begin(), pids.end(), RootPid_);
    if (it != pids.end()) {
        pids.erase(it);
    }

    YT_LOG_DEBUG("Running strace (Pids: %v)", pids);

    auto result = WaitFor(BIND([=] () {
        return RunTool<TStraceTool>(pids);
    }).AsyncVia(AuxQueue_->GetInvoker()).Run());

    THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error running job strace tool");

    return ConvertToYsonString(result.Value());
}

void TJobProbeTools::SignalJob(const TString& signalName)
{
    auto arg = New<TSignalerConfig>();
    arg->Pids = PidsHolder_->GetPids();

    YT_LOG_DEBUG("Processing \"SignalJob\" (Signal: %v, Pids: %v, RootPid: %v)",
        signalName,
        arg->Pids,
        RootPid_);

    auto it = std::find(arg->Pids.begin(), arg->Pids.end(), RootPid_);
    if (it != arg->Pids.end()) {
        arg->Pids.erase(it);
    }

    if (arg->Pids.empty()) {
        return;
    }

    arg->SignalName = signalName;

    YT_LOG_INFO("Sending signal (Signal: %v, Pids: %v)",
        arg->SignalName,
        arg->Pids);

    auto result = WaitFor(BIND([=] () {
        return RunTool<TSignalerTool>(arg);
    }).AsyncVia(AuxQueue_->GetInvoker()).Run());

    THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error running job signaler tool");
}

TYsonString TJobProbeTools::PollJobShell(const TYsonString& parameters)
{
    return WaitFor(BIND([=, this_ = MakeStrong(this)] () {
        return ShellManager_->PollJobShell(parameters);
    }).AsyncVia(AuxQueue_->GetInvoker()).Run()).ValueOrThrow();
}

TFuture<void> TJobProbeTools::AsyncGracefulShutdown(const TError& error)
{
    return BIND(&IShellManager::GracefulShutdown, ShellManager_, error)
        .AsyncVia(AuxQueue_->GetInvoker())
        .Run();
}

////////////////////////////////////////////////////////////////////////////////

class TJobSatelliteWorker
    : public IJobProbe
{
public:
    TJobSatelliteWorker(
        pid_t rootPid,
        int uid,
        const std::vector<TString>& env,
        TJobId jobId,
        EJobEnvironmentType environmentType,
        bool enableSecureVaultVariablesInJobShell);
    void GracefulShutdown(const TError& error);

    virtual std::vector<NChunkClient::TChunkId> DumpInputContext() override;
    virtual TYsonString StraceJob() override;
    virtual void SignalJob(const TString& signalName) override;
    virtual TYsonString PollJobShell(const TYsonString& parameters) override;
    virtual TString GetStderr() override;
    virtual void Interrupt() override;
    virtual void Fail() override;

private:
    const pid_t RootPid_;
    const int Uid_;
    const std::vector<TString> Env_;
    const TJobId JobId_;
    const EJobEnvironmentType EnvironmentType_;
    const bool EnableSecureVaultVariablesInJobShell_;

    const NLogging::TLogger Logger;

    TJobProbeToolsPtr JobProbe_;

    void EnsureJobProbe();
};

TJobSatelliteWorker::TJobSatelliteWorker(
    pid_t rootPid,
    int uid,
    const std::vector<TString>& env,
    TJobId jobId,
    EJobEnvironmentType environmentType,
    bool enableSecureVaultVariablesInJobShell)
    : RootPid_(rootPid)
    , Uid_(uid)
    , Env_(env)
    , JobId_(jobId)
    , EnvironmentType_(environmentType)
    , EnableSecureVaultVariablesInJobShell_(enableSecureVaultVariablesInJobShell)
    , Logger(NLogging::TLogger(JobSatelliteLogger)
        .AddTag("JobId: %v", JobId_))
{
    YT_VERIFY(JobId_);
    YT_LOG_DEBUG("Starting job satellite service");
}

void TJobSatelliteWorker::EnsureJobProbe()
{
    if (!JobProbe_) {
        JobProbe_ = TJobProbeTools::Create(JobId_, RootPid_, Uid_, Env_, EnvironmentType_, EnableSecureVaultVariablesInJobShell_);
    }
}

std::vector<TChunkId> TJobSatelliteWorker::DumpInputContext()
{
    YT_ABORT();
}

TYsonString TJobSatelliteWorker::StraceJob()
{
    EnsureJobProbe();
    return JobProbe_->StraceJob();
}

TString TJobSatelliteWorker::GetStderr()
{
    YT_ABORT();
}

void TJobSatelliteWorker::SignalJob(const TString& signalName)
{
    EnsureJobProbe();
    JobProbe_->SignalJob(signalName);
}

TYsonString TJobSatelliteWorker::PollJobShell(const TYsonString& parameters)
{
    EnsureJobProbe();
    return JobProbe_->PollJobShell(parameters);
}

void TJobSatelliteWorker::Interrupt()
{
    YT_ABORT();
}

void TJobSatelliteWorker::Fail()
{
    YT_ABORT();
}

void TJobSatelliteWorker::GracefulShutdown(const TError &error)
{
    if (JobProbe_) {
        WaitFor(JobProbe_->AsyncGracefulShutdown(error))
            .ThrowOnError();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TJobSatellite
    : public TRefCounted
{
public:
    TJobSatellite(
        TJobSatelliteConnectionConfigPtr config,
        pid_t rootPid,
        int uid,
        const std::vector<TString>& env,
        TJobId jobId);
    void Run();
    void Stop(const TError& error);

private:
    const TJobSatelliteConnectionConfigPtr SatelliteConnectionConfig_;
    const pid_t RootPid_;
    const int Uid_;
    const std::vector<TString> Env_;
    const TJobId JobId_;
    const NConcurrency::TActionQueuePtr JobSatelliteMainThread_;
    NRpc::IServerPtr RpcServer_;
    IUserJobSynchronizerClientPtr JobProxyControl_;
    TCallback<void(const TError& error)> StopCalback_;
};

TJobSatellite::TJobSatellite(TJobSatelliteConnectionConfigPtr config,
    pid_t rootPid,
    int uid,
    const std::vector<TString>& env,
    TJobId jobId)
    : SatelliteConnectionConfig_(config)
    , RootPid_(rootPid)
    , Uid_(uid)
    , Env_(env)
    , JobId_(jobId)
    , JobSatelliteMainThread_(New<TActionQueue>("JobSatelliteMain"))
{ }

void TJobSatellite::Stop(const TError& error)
{
    StopCalback_.Run(error);
    JobProxyControl_->NotifyUserJobFinished(error);
    RpcServer_->Stop().Get();
}

void TJobSatellite::Run()
{
    JobProxyControl_ = CreateUserJobSynchronizerClient(SatelliteConnectionConfig_->JobProxyRpcClientConfig);

    RpcServer_ = NRpc::NBus::CreateBusServer(CreateTcpBusServer(SatelliteConnectionConfig_->SatelliteRpcServerConfig));

    auto jobSatelliteService = New<TJobSatelliteWorker>(
        RootPid_,
        Uid_,
        Env_,
        JobId_,
        SatelliteConnectionConfig_->EnvironmentType,
        SatelliteConnectionConfig_->EnableSecureVaultVariablesInJobShell
    );

    RpcServer_->RegisterService(CreateJobProberService(jobSatelliteService, JobSatelliteMainThread_->GetInvoker()));
    RpcServer_->Start();

    StopCalback_ = BIND(&TJobSatelliteWorker::GracefulShutdown,
        MakeWeak(jobSatelliteService));

    i64 rss = 0;
    try {
        rss = GetProcessMemoryUsage(-1).Rss;
    } catch (const std::exception& ex) {
        YT_LOG_WARNING(ex, "Failed to get process memory usage");
    }

    JobProxyControl_->NotifyJobSatellitePrepared(rss);
}

////////////////////////////////////////////////////////////////////////////////

void RunJobSatellite(
    TJobSatelliteConnectionConfigPtr config,
    const int uid,
    const std::vector<TString>& env,
    const TString& jobId)
{
    pid_t pid = fork();
    if (pid == -1) {
        THROW_ERROR_EXCEPTION("Cannot fork")
            << TError::FromSystem();
    } else if (pid == 0) { // child
        return;
    } else {

        NLogging::TLogManager::Get()->Configure(NLogging::TLogManagerConfig::CreateLogFile("../job_satellite.log"));
        try {
            SafeCreateStderrFile("../satellite_stderr");
        } catch (const std::exception& ex) {
            YT_LOG_ERROR("Failed to reopen satellite stderr");
            _exit(1);
        }

        siginfo_t processInfo;
        memset(&processInfo, 0, sizeof(siginfo_t));
        try {
            auto jobSatellite = New<TJobSatellite>(config, pid, uid, env, TJobId::FromString(jobId));
            jobSatellite->Run();

            YT_VERIFY(HandleEintr(::waitid, P_PID, pid, &processInfo, WEXITED) == 0);

            jobSatellite->Stop(ProcessInfoToError(processInfo));
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Exception thrown during job satellite functioning");
            _exit(1);
        }
        YT_LOG_DEBUG("User process finished (Pid: %v, Status: %v)",
            pid,
            ProcessInfoToError(processInfo));
        NLogging::TLogManager::StaticShutdown();
        _exit(0);
    }
}

void NotifyExecutorPrepared(TJobSatelliteConnectionConfigPtr config)
{
    try {
        auto jobProxyControl = CreateUserJobSynchronizerClient(config->JobProxyRpcClientConfig);
        jobProxyControl->NotifyExecutorPrepared();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error notifying job proxy")
            << ex;
    }
    Shutdown();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
