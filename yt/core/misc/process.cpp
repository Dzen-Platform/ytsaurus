#include "process.h"
#include "proc.h"

#include <yt/core/logging/log.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/fs.h>

#include <util/system/execpath.h>

#ifdef _unix_
  #include <unistd.h>
  #include <errno.h>
  #include <sys/wait.h>
#endif

#ifdef _darwin_
  #include <crt_externs.h>
  #define environ (*_NSGetEnviron())
#endif

namespace NYT {

using namespace NPipes;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("Process");

static const pid_t InvalidProcessId = -1;

////////////////////////////////////////////////////////////////////////////////

namespace {

#ifdef _unix_

bool TryKill(int pid, int signal)
{
    YCHECK(pid > 0);
    int result = ::kill(pid, signal);
    // Ignore ESRCH because process may have died just before TryKill.
    if (result < 0 && errno != ESRCH) {
        return false;
    }
    return true;
}

bool TryWaitid(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
    while (true) {
        if (infop != nullptr) {
            // See comment below.
            infop->si_pid = 0;
        }

        siginfo_t info;
        ::memset(&info, 0, sizeof(info));
        auto res = ::waitid(idtype, id, infop != nullptr ? infop : &info, options);

        if (res == 0) {
            // According to man wait.
            // If WNOHANG was specified in options and there were
            // no children in a waitable state, then waitid() returns 0 immediately.
            // To distinguish this case from that where a child
            // was in a waitable state, zero out the si_pid field
            // before the call and check for a nonzero value in this field after
            // the call returns.
            if (infop && infop->si_pid == 0) {
                return false;
            }
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }
}

void WaitidOrDie(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
    YCHECK(infop != nullptr);

    memset(infop, 0, sizeof(siginfo_t));

    bool isOK = TryWaitid(idtype, id, infop, options);

    if (!isOK) {
        LOG_FATAL(TError::FromSystem(), "Waitid failed with options: %v", options);
    }

    YCHECK(infop->si_pid == id);
}

void Cleanup(int pid)
{
    YCHECK(pid > 0);

    YCHECK(TryKill(pid, 9));
    YCHECK(TryWaitid(P_PID, pid, nullptr, WEXITED));
}

bool TrySetSignalMask(const sigset_t* sigmask, sigset_t* oldSigmask)
{
    int error = pthread_sigmask(SIG_SETMASK, sigmask, oldSigmask);
    if (error != 0) {
        return false;
    }
    return true;
}

bool TryResetSignals()
{
    for (int sig = 1; sig < NSIG; ++sig) {
        // Ignore invalid signal errors.
        ::signal(sig, SIG_DFL);
    }
    return true;
}

#endif

} // namespace

////////////////////////////////////////////////////////////////////////////////

TProcess::TProcess(const Stroka& path, bool copyEnv, TDuration pollPeriod)
    // Stroka is guaranteed to be zero-terminated.
    // https://wiki.yandex-team.ru/Development/Poisk/arcadia/util/StrokaAndTStringBuf#sobstvennosimvoly
    : Path_(path)
    , PollPeriod_(pollPeriod)
    , ProcessId_(InvalidProcessId)
{
    AddArgument(NFS::GetFileName(path));

    if (copyEnv) {
        for (char** envIt = environ; *envIt; ++envIt) {
            Env_.push_back(Capture(*envIt));
        }
    }
}

void TProcess::AddArgument(TStringBuf arg)
{
    YCHECK(ProcessId_ == InvalidProcessId && !Finished_);

    Args_.push_back(Capture(arg));
}

void TProcess::AddEnvVar(TStringBuf var)
{
    YCHECK(ProcessId_ == InvalidProcessId && !Finished_);

    Env_.push_back(Capture(var));
}

void TProcess::AddArguments(std::initializer_list<TStringBuf> args)
{
    for (auto arg : args) {
        AddArgument(arg);
    }
}

void TProcess::AddArguments(const std::vector<Stroka>& args)
{
    for (const auto& arg : args) {
        AddArgument(arg);
    }
}

void TProcess::AddCloseFileAction(int fd)
{
    TSpawnAction action{
        std::bind(TryClose, fd, true),
        Format("Error closing %v file descriptor in child process", fd)
    };

    MaxSpawnActionFD_ = std::max(MaxSpawnActionFD_, fd);
    SpawnActions_.push_back(action);
}

void TProcess::AddDup2FileAction(int oldFD, int newFD)
{
    TSpawnAction action{
        std::bind(TryDup2, oldFD, newFD),
        Format("Error duplicating %v file descriptor to %v in child process", oldFD, newFD)
    };

    MaxSpawnActionFD_ = std::max(MaxSpawnActionFD_, newFD);
    SpawnActions_.push_back(action);
}

TFuture<void> TProcess::Spawn()
{
    try {
        DoSpawn();
    } catch (const std::exception& ex) {
        FinishedPromise_.TrySet(ex);
    }
    return FinishedPromise_;
}

void TProcess::DoSpawn()
{
#ifdef _unix_
    YCHECK(ProcessId_ == InvalidProcessId && !Finished_);

    // Make sure no spawn action closes Pipe_.WriteFD
    TPipeFactory pipeFactory(MaxSpawnActionFD_ + 1);
    Pipe_ = pipeFactory.Create();
    pipeFactory.Clear();

    LOG_DEBUG("Spawning new process (Path: %v, ErrorPipe: %v,  Arguments: %v, Environment: %v)",
        Path_,
        Pipe_,
        Args_,
        Env_);

    Env_.push_back(nullptr);
    Args_.push_back(nullptr);

    // Block all signals around vfork; see http://ewontfix.com/7/

    // As the child may run in the same address space as the parent until
    // the actual execve() system call, any (custom) signal handlers that
    // the parent has might alter parent's memory if invoked in the child,
    // with undefined results.  So we block all signals in the parent before
    // vfork(), which will cause them to be blocked in the child as well (we
    // rely on the fact that Linux, just like all sane implementations, only
    // clones the calling thread).  Then, in the child, we reset all signals
    // to their default dispositions (while still blocked), and unblock them
    // (so the exec()ed process inherits the parent's signal mask)

    sigset_t allBlocked;
    sigfillset(&allBlocked);
    sigset_t oldSignals;

    if (!TrySetSignalMask(&allBlocked, &oldSignals)) {
        THROW_ERROR_EXCEPTION("Failed to block all signals")
            << TError::FromSystem();
    }

    SpawnActions_.push_back(TSpawnAction{
        TryResetSignals,
        "Error resetting signals to default disposition in child process: signal failed"
    });

    SpawnActions_.push_back(TSpawnAction{
        std::bind(TrySetSignalMask, &oldSignals, nullptr),
        "Error unblocking signals in child process: pthread_sigmask failed"
    });

    SpawnActions_.push_back(TSpawnAction{
        std::bind(TryExecve, Path_.c_str(), Args_.data(), Env_.data()),
        "Error starting child process: execve failed"
    });

    SpawnChild();

    // This should not fail ever.
    YCHECK(TrySetSignalMask(&oldSignals, nullptr));

    Pipe_.CloseWriteFD();
    ValidateSpawnResult();

    AsyncWaitExecutor_ = New<TPeriodicExecutor>(
        GetSyncInvoker(),
        BIND(&TProcess::AsyncPeriodicTryWait, MakeStrong(this)),
        PollPeriod_);

    AsyncWaitExecutor_->Start();
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

void TProcess::SpawnChild()
{
#ifdef _unix_
    int pid = vfork();

    if (pid < 0) {
        THROW_ERROR_EXCEPTION("Error starting child process: vfork failed")
            << TErrorAttribute("path", Path_)
            << TError::FromSystem();
    }

    if (pid == 0) {
        try {
            Child();
        } catch (...) {
            YUNREACHABLE();
        }
    }

    ProcessId_ = pid;
    Started_ = true;
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

void TProcess::ValidateSpawnResult()
{
#ifdef _unix_
    int data[2];
    int res = ::read(Pipe_.GetReadFD(), &data, sizeof(data));
    Pipe_.CloseReadFD();

    if (res == 0) {
        // Child successfully spawned or was killed by a signal.
        // But there is no way to distinguish between these two cases:
        // * child killed by signal before exec
        // * child killed by signal after exec
        // So we treat kill-before-exec the same way as kill-after-exec.
        LOG_DEBUG("Child process spawned successfully (Pid: %v)", ProcessId_);
        return;
    }

    YCHECK(res == sizeof(data));
    Finished_ = true;

    Cleanup(ProcessId_);
    ProcessId_ = InvalidProcessId;

    int actionIndex = data[0];
    int errorCode = data[1];

    YCHECK(0 <= actionIndex && actionIndex < SpawnActions_.size());
    const auto& action = SpawnActions_[actionIndex];
    THROW_ERROR_EXCEPTION("%v", action.ErrorMessage)
        << TError::FromSystem(errorCode);
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

#ifdef _unix_
static TError ProcessInfoToError(const siginfo_t& processInfo)
{
    switch (processInfo.si_code) {
        case CLD_EXITED: {
            auto exitCode = processInfo.si_status;
            if (exitCode == 0) {
                return TError();
            } else {
                return TError(
                    EProcessErrorCode::NonZeroExitCode,
                    "Process exited with code %v",
                    exitCode)
                    << TErrorAttribute("exit_code", exitCode);
            }
        }

        case CLD_KILLED:
        case CLD_DUMPED: {
            int signal = processInfo.si_status;
            return TError(
                EProcessErrorCode::Signal,
                "Process terminated by signal %v",
                signal)
                << TErrorAttribute("signal", signal);
        }

        default:
            return TError("Unknown signal code %v",
                processInfo.si_code);
    }
}
#endif

#ifdef _unix_
void TProcess::AsyncPeriodicTryWait()
{
    siginfo_t processInfo;
    memset(&processInfo, 0, sizeof(siginfo_t));

    // Note WNOWAIT flag.
    // This call just waits for a process to be finished but does not clear zombie flag.

    if (!TryWaitid(P_PID, ProcessId_, &processInfo, WEXITED | WNOWAIT | WNOHANG) ||
        processInfo.si_pid != ProcessId_)
    {
        return;
    }

    AsyncWaitExecutor_->Stop();
    AsyncWaitExecutor_ = nullptr;

    // This call just should return immediately
    // because we have already waited for this process with WNOHANG
    WaitidOrDie(P_PID, ProcessId_, &processInfo, WEXITED | WNOHANG);

    Finished_ = true;
    LOG_DEBUG("Process finished (Pid: %v)", ProcessId_);

    FinishedPromise_.Set(ProcessInfoToError(processInfo));
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

void TProcess::Kill(int signal)
{
#ifdef _unix_
    if (!Started_) {
        THROW_ERROR_EXCEPTION("Process is not started yet");
    }

    if (Finished_) {
        return;
    }

    LOG_DEBUG("Killing child process (Pid: %v)", ProcessId_);

    auto result = TryKill(ProcessId_, signal);
    if (!result) {
        THROW_ERROR_EXCEPTION("Failed to kill child process %v", ProcessId_)
            << TError::FromSystem();
    }
    return;
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

Stroka TProcess::GetPath() const
{
    return Path_;
}

int TProcess::GetProcessId() const
{
    return ProcessId_;
}

bool TProcess::IsStarted() const
{
    return Started_;
}

bool TProcess::IsFinished() const
{
    return Finished_;
}

Stroka TProcess::GetCommandLine() const
{
    TStringBuilder builder;
    builder.AppendString(Path_);

    bool first = true;
    for (auto arg : Args_) {
        if (first) {
            first = false;
        } else {
            if (arg) {
                builder.AppendChar(' ');
                bool needQuote = (TStringBuf(arg).find(' ') != Stroka::npos);
                if (needQuote) {
                    builder.AppendChar('"');
                }
                builder.AppendString(arg);
                if (needQuote) {
                    builder.AppendChar('"');
                }
            }
        }
    }
    return builder.Flush();
}

const char* TProcess::Capture(const TStringBuf& arg)
{
    StringHolders_.push_back(Stroka(arg));
    return StringHolders_.back().c_str();
}

void TProcess::Child()
{
#ifdef _unix_
    for (int actionIndex = 0; actionIndex < SpawnActions_.size(); ++actionIndex) {
        auto& action = SpawnActions_[actionIndex];
        if (!action.Callback()) {
            // Report error through the pipe.
            int data[] = {
                actionIndex,
                errno
            };

            // According to pipe(7) write of small buffer is atomic.
            YCHECK(::write(Pipe_.GetWriteFD(), &data, sizeof(data)) == sizeof(data));
            _exit(1);
        }
    }
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
    YUNREACHABLE();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
