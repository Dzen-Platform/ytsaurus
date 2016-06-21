#include "proc.h"
#include "process.h"
#include "string.h"

#include <yt/core/logging/log.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/fs.h>
#include <yt/core/misc/string.h>

#include <yt/core/tools/registry.h>

#include <yt/core/ytree/convert.h>

#include <util/stream/file.h>

#include <util/string/vector.h>

#include <util/system/info.h>
#include <util/system/yield.h>

#ifdef _unix_
    #include <stdio.h>
    #include <dirent.h>
    #include <errno.h>
    #include <pwd.h>
    #include <sys/ioctl.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif
#ifdef _linux_
    #include <pty.h>
    #include <utmp.h>
#endif
#ifdef _darwin_
    //include <util.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("Proc");

////////////////////////////////////////////////////////////////////////////////

std::vector<int> GetPidsByUid(int uid)
{
#ifdef _linux_
    std::vector<int> result;

    DIR *dirStream = ::opendir("/proc");
    YCHECK(dirStream != nullptr);

    struct dirent *ep;
    while ((ep = ::readdir(dirStream)) != nullptr) {
        const char* begin = ep->d_name;
        char* end = nullptr;
        int pid = static_cast<int>(strtol(begin, &end, 10));
        if (begin == end) {
            // Not a pid.
            continue;
        }

        auto path = Format("/proc/%v", pid);
        struct stat buf;
        int res = ::stat(~path, &buf);

        if (res == 0) {
            if (buf.st_uid == uid) {
                result.push_back(pid);
            }
        } else {
            // Assume that the process has already completed.
            auto errno_ = errno;
            LOG_DEBUG(TError::FromSystem(), "Failed to get UID for PID %v: stat failed",
                pid);
            YCHECK(errno_ == ENOENT || errno_ == ENOTDIR);
        }
    }

    YCHECK(::closedir(dirStream) == 0);
    return result;

#else
    return std::vector<int>();
#endif
}

i64 GetProcessRss(int pid)
{
#ifdef _linux_
    Stroka path = "/proc/self/statm";
    if (pid != -1) {
        path = Format("/proc/%v/statm", pid);
    }

    TIFStream memoryStatFile(path);
    auto memoryStatFields = splitStroku(memoryStatFile.ReadLine(), " ");
    return FromString<i64>(memoryStatFields[1]) * NSystemInfo::GetPageSize();
#else
    return 0;
#endif
}

Stroka GetProcessName(int pid)
{
#ifdef _linux_
    Stroka path = Format("/proc/%v/comm", pid);
    return Trim(TFileInput(path).ReadAll(), "\n");
#else
    return "";
#endif
}

std::vector<Stroka> GetProcessCommandLine(int pid)
{
#ifdef _linux_
    Stroka path = Format("/proc/%v/cmdline", pid);
    auto raw = TFileInput(path).ReadAll();
    std::vector<Stroka> result;
    auto begin = 0;
    while (begin < raw.length()) {
        auto end = raw.find('\0', begin);
        if (end == Stroka::npos) {
            result.push_back(raw.substr(begin));
            begin = raw.length();
        } else {
            result.push_back(raw.substr(begin, end - begin));
            begin = end + 1;
        }
    }

    return result;
#else
    return std::vector<Stroka>();
#endif
}

#ifdef _unix_

void RemoveDirAsRoot(const Stroka& path)
{
    // Child process
    SafeSetUid(0);
    execl("/bin/rm", "/bin/rm", "-rf", path.c_str(), (void*)nullptr);

    THROW_ERROR_EXCEPTION("Failed to remove directory %Qv: execl failed",
        path) << TError::FromSystem();
}

void MountTmpfsAsRoot(TMountTmpfsConfigPtr config)
{
    SafeSetUid(0);
    NFS::MountTmpfs(config->Path, config->UserId, config->Size);
}

void UmountAsRoot(const Stroka& path)
{
    SafeSetUid(0);
    NFS::Umount(path);
}

TError StatusToError(int status)
{
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
        return TError();
    } else if (WIFSIGNALED(status)) {
        int signalNumber = WTERMSIG(status);
        return TError(
            EProcessErrorCode::Signal,
            "Process terminated by signal %v",
            signalNumber)
            << TErrorAttribute("signal", signalNumber);
    } else if (WIFSTOPPED(status)) {
        int signalNumber = WSTOPSIG(status);
        return TError(
            EProcessErrorCode::Signal,
            "Process stopped by signal %v",
            signalNumber)
            << TErrorAttribute("signal", signalNumber);
    } else if (WIFEXITED(status)) {
        int exitCode = WEXITSTATUS(status);
        return TError(
            EProcessErrorCode::NonZeroExitCode,
            "Process exited with code %v",
            exitCode)
            << TErrorAttribute("exit_code", exitCode);
    } else {
        return TError("Unknown status %v", status);
    }
}

bool TryExecve(const char* path, const char* const* argv, const char* const* env)
{
    ::execve(
        path,
        const_cast<char* const*>(argv),
        const_cast<char* const*>(env));
    // If we are still here, it's an error.
    return false;
}

bool TryDup2(int oldFD, int newFD)
{
    while (true) {
        auto res = HandleEintr(::dup2, oldFD, newFD);

        if (res != -1) {
            return true;
        }

        if (errno == EBUSY) {
            continue;
        }

        return false;
    }
}

bool TryClose(int fd, bool ignoreBadFD)
{
    while (true) {
        auto res = ::close(fd);
        if (res != -1) {
            return true;
        }

        switch (errno) {
            // Please read
            // http://lkml.indiana.edu/hypermail/linux/kernel/0509.1/0877.html and
            // http://rb.yandex-team.ru/arc/r/44030/
            // before editing.
            case EINTR:
                return true;
            case EBADF:
                return ignoreBadFD;
            default:
                return false;
        }
    }
}

void SafeClose(int fd, bool ignoreBadFD)
{
    if (!TryClose(fd, ignoreBadFD)) {
        THROW_ERROR TError::FromSystem();
    }
}

void SafeDup2(int oldFD, int newFD)
{
    if (!TryDup2(oldFD, newFD)) {
        THROW_ERROR_EXCEPTION("dup2 failed")
            << TErrorAttribute("old_fd", oldFD)
            << TErrorAttribute("new_fd", newFD)
            << TError::FromSystem();
    }
}

void SafeSetCloexec(int fd)
{
    int getResult = ::fcntl(fd, F_GETFD);
    if (getResult == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe: fcntl failed to get descriptor flags")
            << TError::FromSystem();
    }

    int setResult = ::fcntl(fd, F_SETFD, getResult | FD_CLOEXEC);
    if (setResult == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe: fcntl failed to set descriptor flags")
            << TError::FromSystem();
    }
}

void SetPermissions(int fd, int permissions)
{
#ifdef _linux_
    auto procPath = Format("/proc/self/fd/%v", fd);
    auto res = chmod(~procPath, permissions);

    if (res == -1) {
        THROW_ERROR_EXCEPTION("Failed to set permissions for descriptor")
            << TErrorAttribute("fd", fd)
            << TErrorAttribute("permissions", permissions)
            << TError::FromSystem();
    }
#endif
}

void SafePipe(int fd[2])
{
#ifdef _linux_
    auto result = ::pipe2(fd, O_CLOEXEC);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe")
            << TError::FromSystem();
    }
#else
    {
        int result = ::pipe(fd);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error creating pipe: pipe creation failed")
                << TError::FromSystem();
        }
    }
    SafeSetCloexec(fd[0]);
    SafeSetCloexec(fd[1]);
#endif
}

int SafeDup(int fd)
{
    auto result = ::dup(fd);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error duplicating fd")
            << TError::FromSystem();
    }
    return result;
}

void SafeOpenPty(int* masterFD, int* slaveFD, int height, int width)
{
#if 0
    {
        struct termios tt = { };
        tt.c_iflag = TTYDEF_IFLAG & ~ISTRIP;
        tt.c_oflag = TTYDEF_OFLAG;
        tt.c_lflag = TTYDEF_LFLAG;
        tt.c_cflag = (TTYDEF_CFLAG & ~(CS7|PARENB|HUPCL)) | CS8;
        tt.c_cc[VERASE] = '\x7F';
        cfsetispeed(&tt, B38400);
        cfsetospeed(&tt, B38400);

        struct winsize ws = { };
        struct winsize* wsPtr = nullptr;
        if (height > 0 && width > 0) {
            ws.ws_row = height;
            ws.ws_col = width;
            wsPtr = &ws;
        }

        int result = ::openpty(masterFD, slaveFD, nullptr, &tt, wsPtr);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error creating pty: pty creation failed")
                << TError::FromSystem();
        }
    }
    SafeSetCloexec(*masterFD);
#else
    THROW_ERROR_EXCEPTION("Unsupported");
#endif
}

void SafeLoginTty(int slaveFD)
{
#if 0
    int result = ::login_tty(slaveFD);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error attaching pty to standard streams")
            << TError::FromSystem();
    }
#else
    THROW_ERROR_EXCEPTION("Unsupported");
#endif
}

void SafeSetTtyWindowSize(int fd, int height, int width)
{
    if (height > 0 && width > 0) {
        struct winsize ws;
        int result = ::ioctl(fd, TIOCGWINSZ, &ws);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error reading tty window size")
                << TError::FromSystem();
        }
        if (ws.ws_row != height || ws.ws_col != width) {
            ws.ws_row = height;
            ws.ws_col = width;
            result = ::ioctl(fd, TIOCSWINSZ, &ws);
            if (result == -1) {
                THROW_ERROR_EXCEPTION("Error setting tty window size")
                    << TError::FromSystem();
            }
        }
    }
}

bool TryMakeNonblocking(int fd)
{
    auto res = fcntl(fd, F_GETFL);

    if (res == -1) {
        return false;
    }

    res = fcntl(fd, F_SETFL, res | O_NONBLOCK);

    if (res == -1) {
        return false;
    }

    return true;
}

void SafeMakeNonblocking(int fd)
{
    if (!TryMakeNonblocking(fd)) {
        THROW_ERROR_EXCEPTION("Failed to set nonblocking mode for descriptor %v", fd)
            << TError::FromSystem();
    }
}

void SafeSetUid(int uid)
{
    if (setuid(uid) != 0) {
        THROW_ERROR_EXCEPTION("setuid failed to set uid to %v", uid)
            << TError::FromSystem();
    }
}

Stroka SafeGetUsernameByUid(int uid)
{
    int bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize < 0) {
        THROW_ERROR_EXCEPTION("Failed to get username, sysconf(_SC_GETPW_R_SIZE_MAX) failed")
            << TError::FromSystem();
    }
    char buffer[bufferSize];
    struct passwd pwd, *pwdptr = nullptr;
    int result = getpwuid_r(uid, &pwd, buffer, bufferSize, &pwdptr);
    if (result != 0 || pwdptr == nullptr) {
        // Return #uid in case of absent uid in the system.
        return "#" + ToString(uid);
    }
    return pwdptr->pw_name;
}

#else

bool TryClose(int /* fd */, bool /* ignoreBadFD */)
{
    YUNIMPLEMENTED();
}

void SafeClose(int /* fd */, bool /* ignoreBadFD */)
{
    YUNIMPLEMENTED();
}

bool TryDup2(int /* oldFD */, int /* newFD */)
{
    YUNIMPLEMENTED();
}

void SafeDup2(int /* oldFD */, int /* newFD */)
{
    YUNIMPLEMENTED();
}

void SafeSetCloexec(int /* fd */)
{
    YUNIMPLEMENTED();
}

bool TryExecve(const char /* *path */, const char* /* argv[] */, const char* /* env[] */)
{
    YUNIMPLEMENTED();
}

TError StatusToError(int /* status */)
{
    YUNIMPLEMENTED();
}

void RemoveDirAsRoot(const Stroka& /* path */)
{
    YUNIMPLEMENTED();
}

void MountTmpfsAsRoot(TMountTmpfsConfigPtr /* config */)
{
    YUNIMPLEMENTED();
}

void UmountAsRoot(const Stroka& /* path */)
{
    YUNIMPLEMENTED();
}

void CloseAllDescriptors()
{
    YUNIMPLEMENTED();
}

void SetPermissions(int /* fd */, int /* permissions */)
{
    YUNIMPLEMENTED();
}

void SafePipe(int /* fd */ [2])
{
    YUNIMPLEMENTED();
}

int SafeDup(int /* fd */)
{
    YUNIMPLEMENTED();
}

void SafeOpenPty(int* /* masterFD */, int* /* slaveFD */, int /* height */, int /* width */)
{
    YUNIMPLEMENTED();
}

void SafeLoginTty(int /* slaveFD */)
{
    YUNIMPLEMENTED();
}

void SafeSetTtyWindowSize(int /* slaveFD */, int /* height */, int /* width */)
{
    YUNIMPLEMENTED();
}

bool TryMakeNonblocking(int /* fd */)
{
    YUNIMPLEMENTED();
}

void SafeMakeNonblocking(int /* fd */)
{
    YUNIMPLEMENTED();
}

void SafeSetUid(int /* uid */)
{
    YUNIMPLEMENTED();
}

Stroka SafeGetUsernameByUid(int /* uid */)
{
    YUNIMPLEMENTED();
}
#endif

void CloseAllDescriptors(const std::vector<int>& exceptFor)
{
#ifdef _linux_
    auto* dirStream = ::opendir("/proc/self/fd");
    YCHECK(dirStream != NULL);

    int dirFD = ::dirfd(dirStream);
    YCHECK(dirFD >= 0);

    dirent* ep;
    while ((ep = ::readdir(dirStream)) != nullptr) {
        char* begin = ep->d_name;
        char* end = nullptr;
        int fd = static_cast<int>(strtol(begin, &end, 10));
        if (begin == end)
            continue;
        if (fd == dirFD)
            continue;
        if (std::find(exceptFor.begin(), exceptFor.end(), fd) != exceptFor.end())
            continue;
        YCHECK(::close(fd) == 0);
    }

    YCHECK(::closedir(dirStream) == 0);
#endif
}

void CreateStderrFile(Stroka fileName)
{
#ifdef _unix_
    YCHECK(freopen(~fileName, "a", stderr) != nullptr);
#endif
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveDirAsRootTool::operator()(const Stroka& arg) const
{
    RemoveDirAsRoot(arg);
}

REGISTER_TOOL(TRemoveDirAsRootTool);

////////////////////////////////////////////////////////////////////////////////

void TMountTmpfsAsRootTool::operator()(TMountTmpfsConfigPtr arg) const
{
    MountTmpfsAsRoot(arg);
}

REGISTER_TOOL(TMountTmpfsAsRootTool);

////////////////////////////////////////////////////////////////////////////////

void TUmountAsRootTool::operator()(const Stroka& arg) const
{
    UmountAsRoot(arg);
}

REGISTER_TOOL(TUmountAsRootTool);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
