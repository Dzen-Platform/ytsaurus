#include "job_satellite.h"

#include <yt/server/lib/exec_agent/public.h>

#include <yt/server/lib/job_prober/job_prober_service.h>

#include <yt/server/lib/job_satellite_connection/job_satellite_connection.h>

#include <yt/server/lib/user_job_synchronizer_client/user_job_synchronizer.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/job_prober_client/job_probe.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/ytlib/tools/tools.h>
#include <yt/ytlib/tools/stracer.h>
#include <yt/ytlib/tools/signaler.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/library/process/process.h>

#include <yt/core/bus/tcp/config.h>
#include <yt/core/bus/tcp/server.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/logging/config.h>
#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/fs.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/shutdown.h>

#include <yt/core/rpc/bus/server.h>
#include <yt/core/rpc/server.h>

#include <yt/core/yson/string.h>

#include <yt/core/ytree/convert.h>

#include <util/generic/guid.h>
#include <util/system/fs.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

namespace NYT::NExec {

using namespace NRpc;
using namespace NYT::NBus;
using namespace NConcurrency;
using namespace NJobProber;
using namespace NJobSatelliteConnection;
using namespace NTools;
using namespace NUserJobSynchronizerClient;

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

class TRootlessPidsHolder
    : public IPidsHolder
{
public:
    explicit TRootlessPidsHolder()
    { }

    virtual std::vector<int> GetPids() override
    {
        return GetPidsUnderParent(getpid());
    }
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
    static TJobProbeToolsPtr Create(
        TJobId jobId,
        pid_t rootPid,
        int uid,
        EJobEnvironmentType environmentType);

    TYsonString StraceJob();
    void SignalJob(const TString& signalName);

private:
    const EJobEnvironmentType EnvironmentType_;
    const pid_t RootPid_;
    const int Uid_;
    const NConcurrency::TActionQueuePtr AuxQueue_;

    std::unique_ptr<IPidsHolder> PidsHolder_;

    std::atomic_flag Stracing_ = ATOMIC_FLAG_INIT;

    TJobProbeTools(pid_t rootPid,
        int uid,
        EJobEnvironmentType environmentType);
    void Init(TJobId jobId);

    DECLARE_NEW_FRIEND();
};

DEFINE_REFCOUNTED_TYPE(TJobProbeTools)

////////////////////////////////////////////////////////////////////////////////

TJobProbeTools::TJobProbeTools(
    pid_t rootPid,
    int uid,
    EJobEnvironmentType environmentType)
    : EnvironmentType_(environmentType)
    , RootPid_(rootPid)
    , Uid_(uid)
    , AuxQueue_(New<TActionQueue>("JobAux"))
{ }

TJobProbeToolsPtr TJobProbeTools::Create(
    TJobId jobId,
    pid_t rootPid,
    int uid,
    EJobEnvironmentType environmentType)
{
    auto tools = New<TJobProbeTools>(rootPid, uid, environmentType);
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
            if (HasRootPermissions()) {
                PidsHolder_.reset(new TSimplePidsHolder(Uid_));
            } else {
                PidsHolder_.reset(new TRootlessPidsHolder());
            }
            break;

        default:
            YT_ABORT();
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

////////////////////////////////////////////////////////////////////////////////

class TJobSatelliteWorker
    : public IJobProbe
{
public:
    TJobSatelliteWorker(
        pid_t rootPid,
        int uid,
        TJobId jobId,
        EJobEnvironmentType environmentType);

    virtual std::vector<NChunkClient::TChunkId> DumpInputContext() override;
    virtual TYsonString StraceJob() override;
    virtual void SignalJob(const TString& signalName) override;
    virtual TYsonString PollJobShell(const TYsonString& /*parameters*/) override;
    virtual TString GetStderr() override;
    virtual void Interrupt() override;
    virtual void Fail() override;

private:
    const pid_t RootPid_;
    const int Uid_;
    const TJobId JobId_;
    const EJobEnvironmentType EnvironmentType_;

    const NLogging::TLogger Logger;

    TJobProbeToolsPtr JobProbe_;

    void EnsureJobProbe();
};

TJobSatelliteWorker::TJobSatelliteWorker(
    pid_t rootPid,
    int uid,
    TJobId jobId,
    EJobEnvironmentType environmentType)
    : RootPid_(rootPid)
    , Uid_(uid)
    , JobId_(jobId)
    , EnvironmentType_(environmentType)
    , Logger(NLogging::TLogger(JobSatelliteLogger)
        .AddTag("JobId: %v", JobId_))
{
    YT_VERIFY(JobId_);
    YT_LOG_DEBUG("Starting job satellite service");
}

void TJobSatelliteWorker::EnsureJobProbe()
{
    if (!JobProbe_) {
        JobProbe_ = TJobProbeTools::Create(JobId_, RootPid_, Uid_, EnvironmentType_);
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

TYsonString TJobSatelliteWorker::PollJobShell(const TYsonString& /*parameters*/)
{
    YT_ABORT();
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

void TJobSatelliteWorker::Interrupt()
{
    YT_ABORT();
}

void TJobSatelliteWorker::Fail()
{
    YT_ABORT();
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
        TJobId jobId);
    void Run();
    void Stop(const TError& error);

private:
    const TJobSatelliteConnectionConfigPtr SatelliteConnectionConfig_;
    const pid_t RootPid_;
    const int Uid_;
    const TJobId JobId_;
    const NConcurrency::TActionQueuePtr JobSatelliteMainThread_;
    NRpc::IServerPtr RpcServer_;
    IUserJobSynchronizerClientPtr JobProxyControl_;
};

TJobSatellite::TJobSatellite(TJobSatelliteConnectionConfigPtr config,
    pid_t rootPid,
    int uid,
    TJobId jobId)
    : SatelliteConnectionConfig_(config)
    , RootPid_(rootPid)
    , Uid_(uid)
    , JobId_(jobId)
    , JobSatelliteMainThread_(New<TActionQueue>("JobSatelliteMain"))
{ }

void TJobSatellite::Stop(const TError& error)
{
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
        JobId_,
        SatelliteConnectionConfig_->EnvironmentType
    );

    RpcServer_->RegisterService(CreateJobProberService(jobSatelliteService, JobSatelliteMainThread_->GetInvoker()));
    RpcServer_->Start();

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
            auto jobSatellite = New<TJobSatellite>(config, pid, uid, TJobId::FromString(jobId));
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

} // namespace NYT::NExec
