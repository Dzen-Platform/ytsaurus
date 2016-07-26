#include "fs.h"
#include "finally.h"

#include <yt/core/logging/log.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/proc.h>

#include <util/folder/dirut.h>
#include <util/folder/filelist.h>

#include <array>

#if defined(_unix_)
    #include <sys/mount.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#endif

#if defined(_linux_)
    #include <mntent.h>
    #include <sys/vfs.h>
#elif defined(_freebsd_) || defined(_darwin_)
    #include <sys/param.h>
    #include <sys/mount.h>
#elif defined (_win_)
    #include <comutil.h>
    #include <shlobj.h>
#endif

namespace NYT {
namespace NFS {

//////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("FS");

//////////////////////////////////////////////////////////////////////////////

namespace {

void ThrowNotSupported()
{
    THROW_ERROR_EXCEPTION("Unsupported platform");
}

} // namespace

bool Exists(const Stroka& path)
{
#ifdef _win32_
    return GetFileAttributesA(~path) != 0xFFFFFFFF;
#else
    return access(~path, F_OK) == 0;
#endif
}

void Remove(const Stroka& path)
{
    bool ok;
#ifdef _win_
    ok = DeleteFileA(~path);
#else
    struct stat sb;
    ok = lstat(~path, &sb) == 0;
    if (ok) {
        if (S_ISDIR(sb.st_mode)) {
            ok = rmdir(~path) == 0;
        } else {
            ok = remove(~path) == 0;
        }
    }
#endif
    if (!ok) {
        THROW_ERROR_EXCEPTION("Cannot remove %v",
            path)
            << TError::FromSystem();
    }
}

void Replace(const Stroka& source, const Stroka& destination)
{
    if (NFS::Exists(destination)) {
        NFS::Remove(destination);
    }
    NFS::Rename(source, destination);
}

void RemoveRecursive(const Stroka& path)
{
    RemoveDirWithContents(path);
}

void Rename(const Stroka& source, const Stroka& destination)
{
    bool ok;
#if defined(_win_)
    ok = MoveFileEx(~source, ~destination, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = rename(~source, ~destination) == 0;
#endif
    if (!ok) {
        THROW_ERROR_EXCEPTION("Cannot rename %v to %v",
            source,
            destination)
            << TError::FromSystem();
    }
}

Stroka GetFileName(const Stroka& path)
{
    size_t slashPosition = path.find_last_of(LOCSLASH_C);
    if (slashPosition == Stroka::npos) {
        return path;
    }
    return path.substr(slashPosition + 1);
}

Stroka GetDirectoryName(const Stroka& path)
{
    auto absPath = CombinePaths(NFs::CurrentWorkingDirectory(), path);
    size_t slashPosition = absPath.find_last_of(LOCSLASH_C);
    return absPath.substr(0, slashPosition);
}

Stroka GetRealPath(const Stroka& path)
{
    auto curPath = CombinePaths(NFs::CurrentWorkingDirectory(), path);
    std::vector<Stroka> parts;
    while (!Exists(curPath)) {
        auto filename = GetFileName(curPath);
        if (filename == ".") {
            // Do nothing.
        } else if (filename == ".." || parts.empty() || parts.back() != "..") {
            parts.push_back(filename);
        } else {
            parts.pop_back();
        }
        curPath = GetDirectoryName(curPath);
    }
    parts.push_back(RealPath(curPath));

#ifdef YT_IN_ARCADIA
    Reverse(parts.begin(), parts.end());
#else
    reverse(parts.begin(), parts.end());
#endif
    return CombinePaths(parts);
}

Stroka GetFileExtension(const Stroka& path)
{
    size_t dotPosition = path.find_last_of('.');
    if (dotPosition == Stroka::npos) {
        return "";
    }
    size_t slashPosition = path.find_last_of(LOCSLASH_C);
    if (slashPosition != Stroka::npos && dotPosition < slashPosition) {
        return "";
    }
    return path.substr(dotPosition + 1);
}

Stroka GetFileNameWithoutExtension(const Stroka& path)
{
    auto fileName = GetFileName(path);
    size_t dotPosition = fileName.find_last_of('.');
    if (dotPosition == Stroka::npos) {
        return fileName;
    }
    return fileName.substr(0, dotPosition);
}

void CleanTempFiles(const Stroka& path)
{
    LOG_INFO("Cleaning temp files in %v", path);

    // TODO(ignat): specify suffix in EnumerateFiles.
    auto entries = EnumerateFiles(path, std::numeric_limits<int>::max());
    for (const auto& entry : entries) {
        if (entry.has_suffix(TempFileSuffix)) {
            auto fileName = NFS::CombinePaths(path, entry);
            LOG_INFO("Removing file %v", fileName);
            NFS::Remove(fileName);
        }
    }
}

std::vector<Stroka> EnumerateFiles(const Stroka& path, int depth)
{
    std::vector<Stroka> result;
    if (NFS::Exists(path)) {
        TFileList list;
        list.Fill(path, TStringBuf(), TStringBuf(), depth);
        int size = list.Size();
        for (int i = 0; i < size; ++i) {
            result.push_back(list.Next());
        }
    }
    return result;
}

std::vector<Stroka> EnumerateDirectories(const Stroka& path, int depth)
{
    std::vector<Stroka> result;
    if (NFS::Exists(path)) {
        TDirsList list;
        list.Fill(path, TStringBuf(), TStringBuf(), depth);
        int size = list.Size();
        for (int i = 0; i < size; ++i) {
            result.push_back(list.Next());
        }
    }
    return result;
}

TDiskSpaceStatistics GetDiskSpaceStatistics(const Stroka& path)
{
    TDiskSpaceStatistics result;
    bool ok;
#ifdef _win_
    ok = GetDiskFreeSpaceEx(
        ~path,
        (PULARGE_INTEGER) &result.AvailableSpace,
        (PULARGE_INTEGER) &result.TotalSpace,
        (PULARGE_INTEGER) &result.FreeSpace) != 0;
#else
    struct statfs fsData;
    ok = statfs(~path, &fsData) == 0;
    result.TotalSpace = (i64) fsData.f_blocks * fsData.f_bsize;
    result.AvailableSpace = (i64) fsData.f_bavail * fsData.f_bsize;
    result.FreeSpace = (i64) fsData.f_bfree * fsData.f_bsize;
#endif

    if (!ok) {
        THROW_ERROR_EXCEPTION("Failed to get disk space statistics for %v",
            path)
            << TError::FromSystem();
    }

    return result;
}

void ForcePath(const Stroka& path, int mode)
{
    MakePathIfNotExist(~path, mode);
}

TFileStatistics GetFileStatistics(const Stroka& path)
{
    TFileStatistics statistics;
#ifdef _unix_
    struct stat fileStat;
    int result = ::stat(~path, &fileStat);
#else
    WIN32_FIND_DATA findData;
    HANDLE handle = ::FindFirstFileA(~path, &findData);
#endif

#ifdef _unix_
    if (result == -1) {
#else
    if (handle == INVALID_HANDLE_VALUE) {
#endif
        THROW_ERROR_EXCEPTION("Failed to get statistics for %v",
            path)
            << TError::FromSystem();
    }

#ifdef _unix_
    statistics.Size = static_cast<i64>(fileStat.st_size);
    statistics.ModificationTime = TInstant::Seconds(fileStat.st_mtime);
    statistics.AccessTime = TInstant::Seconds(fileStat.st_atime);
#else
    ::FindClose(handle);
    statistics.Size = (static_cast<i64>(findData.nFileSizeHigh) << 32) + static_cast<i64>(findData.nFileSizeLow);
    statistics.ModificationTime = TInstant::MicroSeconds(ToMicroSeconds(findData.ftLastWriteTime));
    statistics.AccessTime = TInstant::MicroSeconds(ToMicroSeconds(findData.ftLastAccessTime));
#endif

    return statistics;
}

void Touch(const Stroka& path)
{
#ifdef _unix_
    int result = ::utimes(~path, nullptr);
    if (result != 0) {
        THROW_ERROR_EXCEPTION("Failed to touch %v",
            path)
            << TError::FromSystem();
    }
#else
    ThrowNotSupported();
#endif
}

namespace {

#ifdef _win_
    const char PATH_DELIM = '\\';
    const char PATH_DELIM2 = '/';
#else
    const char PATH_DELIM = '/';
    const char PATH_DELIM2 = 0;
#endif

bool IsAbsolutePath(const Stroka& path)
{
    if (path.empty())
        return false;
    if (path[0] == PATH_DELIM)
        return true;
#ifdef _win_
    if (path[0] == PATH_DELIM2)
        return true;
    if (path[0] > 0 && isalpha(path[0]) && path[1] == ':')
        return true;
#endif
    return false;
}

Stroka JoinPaths(const Stroka& path1, const Stroka& path2)
{
    if (path1.empty())
        return path2;
    if (path2.empty())
        return path1;

    auto path = path1;
    int delim = 0;
    if (path1.back() == PATH_DELIM || path1.back() == PATH_DELIM2)
        ++delim;
    if (path2[0] == PATH_DELIM || path2[0] == PATH_DELIM2)
        ++delim;
    if (delim == 0)
        path.append(1, PATH_DELIM);
    path.append(path2, delim == 2 ? 1 : 0, Stroka::npos);
    return path;
}

} // namespace

Stroka CombinePaths(const Stroka& path1, const Stroka& path2)
{
    return IsAbsolutePath(path2) ? path2 : JoinPaths(path1, path2);
}

Stroka CombinePaths(const std::vector<Stroka>& paths)
{
    YCHECK(!paths.empty());
    if (paths.size() == 1) {
        return paths[0];
    }
    auto result = CombinePaths(paths[0], paths[1]);
    for (int index = 2; index < paths.size(); ++ index) {
        result = CombinePaths(result, paths[index]);
    }
    return result;
}

Stroka NormalizePathSeparators(const Stroka& path)
{
    Stroka result;
    result.reserve(path.length());
    for (int i = 0; i < path.length(); ++i) {
        if (path[i] == '\\') {
            result.append('/');
        } else {
            result.append(path[i]);
        }
    }
    return result;
}

void SetExecutableMode(const Stroka& path, bool executable)
{
#ifdef _win_
    Y_UNUSED(path);
    Y_UNUSED(executable);
#else
    int mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    if (executable) {
        mode |= S_IXOTH;
        mode |= S_IXGRP;
        mode |= S_IXUSR;
    }
    bool ok = chmod(~path, mode) == 0;
    if (!ok) {
        THROW_ERROR_EXCEPTION(
            "Failed to set mode %v for %v",
            mode,
            path)
            << TError::FromSystem();
    }
#endif
}

void MakeSymbolicLink(const Stroka& filePath, const Stroka& linkPath)
{
#ifdef _win_
    // From MSDN: If the function succeeds, the return value is nonzero.
    // If the function fails, the return value is zero. To get extended error information, call GetLastError.
    bool ok = CreateSymbolicLink(~linkPath, ~filePath, 0) != 0;
#else
    bool ok = symlink(~filePath, ~linkPath) == 0;
#endif

    if (!ok) {
        THROW_ERROR_EXCEPTION(
            "Failed to link %v to %v",
            filePath,
            linkPath)
            << TError::FromSystem();
    }
}

bool AreInodesIdentical(const Stroka& lhsPath, const Stroka& rhsPath)
{
#ifdef _unix_
    auto checkedStat = [] (const Stroka& path, struct stat* buffer) {
        auto result = stat(~path, buffer);
        if (result) {
            THROW_ERROR_EXCEPTION(
                "Failed to check for identical inodes: stat failed for %v",
                path)
                << TError::FromSystem();
        }
    };

    struct stat lhsBuffer, rhsBuffer;
    checkedStat(lhsPath, &lhsBuffer);
    checkedStat(rhsPath, &rhsBuffer);

    return
        lhsBuffer.st_dev == rhsBuffer.st_dev &&
        lhsBuffer.st_ino == rhsBuffer.st_ino;
#else
    return false;
#endif
}

Stroka GetHomePath()
{
#ifdef _win_
    std::array<char, 1024> buffer;
    SHGetSpecialFolderPath(0, buffer.data(), CSIDL_PROFILE, 0);
    return Stroka(buffer.data());
#else
    return std::getenv("HOME");
#endif
}

void FlushDirectory(const Stroka& path)
{
#ifdef _unix_
    int fd = ::open(~path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        THROW_ERROR_EXCEPTION("Failed to open directory %v", path)
            << TError::FromSystem();
    }

    int result = ::fsync(fd);
    if (result < 0) {
        SafeClose(fd, false);
        THROW_ERROR_EXCEPTION("Failed to flush directory %v", path)
            << TError::FromSystem();
    }

    SafeClose(fd, false);
#else
    // No-op.
#endif
}

std::vector<TMountPoint> GetMountPoints(const Stroka& mountsFile)
{
#ifdef _linux_
    std::unique_ptr<FILE, decltype(&endmntent)> file(::setmntent(~mountsFile, "r"), endmntent);

    if (!file.get()) {
        THROW_ERROR_EXCEPTION("Failed to open mounts file %v", mountsFile);
    }

    std::vector<TMountPoint> mountPoints;

    ::mntent* entry;
    while ((entry = getmntent(file.get()))) {
        TMountPoint point;
        point.Name = entry->mnt_fsname;
        point.Path = entry->mnt_dir;
        mountPoints.push_back(point);
    }

    return mountPoints;
#else
    ThrowNotSupported();
    Y_UNREACHABLE();
#endif
}

void MountTmpfs(const Stroka& path, int userId, i64 size)
{
#ifdef _linux_
    auto opts = Format("mode=0700,uid=%v,size=%v", userId, size);
    int result = ::mount("none", ~path, "tmpfs", 0, ~opts);
    if (result < 0) {
        THROW_ERROR_EXCEPTION("Failed to mount tmpfs at %v", path)
            << TErrorAttribute("user_id", userId)
            << TErrorAttribute("size", size)
            << TError::FromSystem();
    }
#else
    ThrowNotSupported();
#endif
}

void Umount(const Stroka& path)
{
#ifdef _linux_
    int result = ::umount(~path);
    // EINVAL for ::umount means that nothing mounted at this point.
    // ENOENT means 'No such file or directory'.
    if (result < 0 && LastSystemError() != EINVAL && LastSystemError() != ENOENT) {
        THROW_ERROR_EXCEPTION("Failed to umount %v", path)
            << TError::FromSystem();
    }
#else
    ThrowNotSupported();
#endif
}

void ExpectIOErrors(std::function<void()> func)
{
    try {
        func();
    } catch (const TSystemError& ex) {
        auto status = ex.Status();
        if (status == EIO || status == ENOSPC || status == EROFS) {
            throw;
        }
        TError error(ex);
        LOG_FATAL(error,"Unexpected exception thrown during IO operation");
    } catch (...) {
        TError error(CurrentExceptionMessage());
        LOG_FATAL(error, "Unexpected exception thrown during IO operation");
    }
}

void Chmod(const Stroka& path, int mode)
{
#ifdef _linux_
    int result = ::Chmod(~path, mode);
    if (result < 0) {
        THROW_ERROR_EXCEPTION("Failed to change mode of %v", path)
            << TErrorAttribute("mode", Format("%04o", mode))
            << TError::FromSystem();
    }
#else
    ThrowNotSupported();
#endif
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFS
} // namespace NYT
