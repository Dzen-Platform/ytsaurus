#include "stdafx.h"
#include "private.h"
#include "cgroup.h"

#include <core/ytree/fluent.h>
#include <core/ytree/serialize.h>
#include <core/ytree/convert.h>
#include <core/ytree/tree_builder.h>

#include <core/misc/fs.h>
#include <core/misc/error.h>
#include <core/misc/process.h>
#include <core/misc/string.h>

#include <util/system/fs.h>
#include <util/string/split.h>
#include <util/string/strip.h>
#include <util/folder/path.h>
#include <util/system/execpath.h>
#include <util/system/yield.h>

#ifdef _linux_
    #include <unistd.h>
    #include <sys/stat.h>
#endif

namespace NYT {
namespace NCGroup {

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CGroupLogger;
static const Stroka CGroupRootPath("/sys/fs/cgroup");

////////////////////////////////////////////////////////////////////////////////

namespace {

Stroka GetParentFor(const Stroka& type)
{
#ifdef _linux_
    auto rawData = TFileInput("/proc/self/cgroup").ReadAll();
    auto result = ParseProcessCGroups(rawData);
    return result[type];
#else
    return "_parent_";
#endif
}

#ifdef _linux_

std::vector<Stroka> ReadAllValues(const Stroka& fileName)
{
    auto raw = TFileInput(fileName).ReadAll();
    LOG_DEBUG("File %v contains %Qv", fileName, raw);

    yvector<Stroka> values;
    Split(raw.data(), " \n", values);
    return std::vector<Stroka>(values.begin(), values.end());
}

TDuration FromJiffies(i64 jiffies)
{
    static long ticksPerSecond = sysconf(_SC_CLK_TCK);
    return TDuration::MicroSeconds(1000 * 1000 * jiffies / ticksPerSecond);
}

#endif

} // namespace

////////////////////////////////////////////////////////////////////////////////

std::vector<Stroka> GetSupportedCGroups()
{
    std::vector<Stroka> result;
    result.push_back("cpuacct");
    result.push_back("blkio");
    result.push_back("memory");
    return result;
}

// The caller must be sure that it has root privileges.
void RunKiller(const Stroka& processGroupPath)
{
    LOG_INFO("Killing processes in cgroup %v", processGroupPath);

#ifdef _linux_
    TNonOwningCGroup group(processGroupPath);
    if (group.IsNull()) {
        return;
    }

    if (!group.Exists()) {
        LOG_WARNING("Cgroup %v does not exists: stop killer", processGroupPath);
        return;
    }

    group.Lock();

    auto children = group.GetChildren();
    auto pids = group.GetTasks();
    if (children.empty() && pids.empty())
        return;

    TProcess process(GetExecPath());
    process.AddArguments({
        "--killer",
        "--process-group-path",
        processGroupPath
    });

    // We are forking here in order not to give the root privileges to the parent process ever,
    // because we cannot know what the other threads are doing.
    process.Spawn();

    auto error = process.Wait();
    THROW_ERROR_EXCEPTION_IF_FAILED(error);
#endif
}

////////////////////////////////////////////////////////////////////////////////

TNonOwningCGroup::TNonOwningCGroup(const Stroka& fullPath)
    : FullPath_(fullPath)
{ }

TNonOwningCGroup::TNonOwningCGroup(const Stroka& type, const Stroka& name)
    : FullPath_(NFS::CombinePaths(NFS::CombinePaths(NFS::CombinePaths(
        CGroupRootPath,
        type),
        GetParentFor(type)),
        name))
{ }

TNonOwningCGroup::TNonOwningCGroup(TNonOwningCGroup&& other)
    : FullPath_(std::move(other.FullPath_))
{ }

void TNonOwningCGroup::AddTask(int pid) const
{
    LOG_INFO("Add %v to cgroup %v", pid, FullPath_);
    Append("tasks", ToString(pid));
}

void TNonOwningCGroup::AddCurrentTask() const
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto pid = getpid();
    AddTask(pid);
#endif
}

Stroka TNonOwningCGroup::Get(const Stroka& name) const
{
    YCHECK(!IsNull());
    Stroka result;
#ifdef _linux_
    const auto path = GetPath(name);
    result = TBufferedFileInput(path).ReadLine();
#endif
    return result;
}

void TNonOwningCGroup::Set(const Stroka& name, const Stroka& value) const
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto path = GetPath(name);
    TFileOutput output(TFile(path, OpenMode::WrOnly));
    output << value;
#endif
}

void TNonOwningCGroup::Append(const Stroka& name, const Stroka& value) const
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto path = GetPath(name);
    TFileOutput output(TFile(path, OpenMode::ForAppend));
    output << value;
#endif
}

bool TNonOwningCGroup::IsRoot() const
{
    TFsPath path(FullPath_);
    path.Fix();
    return path.GetPath() == CGroupRootPath;
}

bool TNonOwningCGroup::IsNull() const
{
    return FullPath_.Empty();
}

bool TNonOwningCGroup::Exists() const
{
    TFsPath path(FullPath_);
    return path.Exists();
}

std::vector<int> TNonOwningCGroup::GetTasks() const
{
    std::vector<int> results;
    if (!IsNull()) {
#ifdef _linux_
        auto values = ReadAllValues(GetPath("tasks"));
        for (const auto& value : values) {
            int pid = FromString<int>(value);
            results.push_back(pid);
        }
#endif
    }
    return results;
}

const Stroka& TNonOwningCGroup::GetFullPath() const
{
    return FullPath_;
}

std::vector<TNonOwningCGroup> TNonOwningCGroup::GetChildren() const
{
    std::vector<TNonOwningCGroup> result;

    if (IsNull()) {
        return result;
    }

    TFsPath path(FullPath_);
    if (path.Exists()) {
        yvector<TFsPath> children;
        path.List(children);
        for (const auto& child : children) {
            if (child.IsDirectory()) {
                result.emplace_back(child.GetPath());
            }
        }
    }
    return result;
}

void TNonOwningCGroup::EnsureExistance() const
{
    LOG_INFO("Creating cgroup %v", FullPath_);

    YCHECK(!IsNull());

#ifdef _linux_
    NFS::ForcePath(FullPath_, 0755);
#endif
}

void TNonOwningCGroup::Lock() const
{
    Traverse(
        BIND([] (const TNonOwningCGroup& group) { group.DoLock(); }),
        BIND([] (const TNonOwningCGroup& group) {} )
    );
}

void TNonOwningCGroup::Unlock() const
{
    Traverse(
        BIND([] (const TNonOwningCGroup& group) {} ),
        BIND([] (const TNonOwningCGroup& group) { group.DoUnlock(); } )
    );
}

void TNonOwningCGroup::Kill() const
{
    YCHECK(!IsRoot());

    Traverse(
        BIND([] (const TNonOwningCGroup& group) { group.DoKill(); } ),
        BIND([] (const TNonOwningCGroup& group) {} )
    );
}

void TNonOwningCGroup::RemoveAllSubcgroups() const
{
    auto this_ = this;
    Traverse(
        BIND([] (const TNonOwningCGroup& group) {} ),
        BIND([this_] (const TNonOwningCGroup& group) {
            if (this_ != &group) {
                group.DoRemove();
            }
        })
    );
}

void TNonOwningCGroup::DoLock() const
{
    LOG_INFO("Locking cgroup %v", FullPath_);

#ifdef _linux_
    if (!IsNull()) {
        int code = chmod(~FullPath_, S_IRUSR | S_IXUSR);
        YCHECK(code == 0);

        code = chmod(~GetPath("tasks"), S_IRUSR);
        YCHECK(code == 0);
    }
#endif
}

void TNonOwningCGroup::DoUnlock() const
{
    LOG_INFO("Unlocking cgroup %v", FullPath_);

#ifdef _linux_
    if (!IsNull()) {
        int code = chmod(~GetPath("tasks"), S_IRUSR | S_IWUSR);
        YCHECK(code == 0);

        code = chmod(~FullPath_, S_IRUSR | S_IXUSR | S_IWUSR);
        YCHECK(code == 0);
    }
#endif
}

void TNonOwningCGroup::DoKill() const
{
    LOG_DEBUG("Started killing processes in cgroup %v", FullPath_);

#ifdef _linux_
    while (true) {
        auto pids = GetTasks();
        if (pids.empty())
            break;

        LOG_DEBUG("Killing processes (PIDs: [%v])",
            JoinToString(pids));

        for (int pid : pids) {
            auto result = kill(pid, 9);
            if (result == -1) {
                YCHECK(errno == ESRCH);
            }
        }

        ThreadYield();
    }
#endif

    LOG_DEBUG("Finished killing processes in cgroup %v", FullPath_);
}

void TNonOwningCGroup::DoRemove() const
{
    TFsPath(FullPath_).DeleteIfExists();
}

void TNonOwningCGroup::Traverse(
        const TCallback<void(const TNonOwningCGroup&)> preorderAction,
        const TCallback<void(const TNonOwningCGroup&)> postorderAction) const
{
    preorderAction.Run(*this);

    for (const auto& child : GetChildren()) {
        child.Traverse(preorderAction, postorderAction);
    }

    postorderAction.Run(*this);
}

Stroka TNonOwningCGroup::GetPath(const Stroka& filename) const
{
    return NFS::CombinePaths(FullPath_, filename);
}

////////////////////////////////////////////////////////////////////////////////

TCGroup::TCGroup(const Stroka& type, const Stroka& name)
    : TNonOwningCGroup(type, name)
    , Created_(false)
{ }

TCGroup::TCGroup(TCGroup&& other)
    : TNonOwningCGroup(std::move(other))
    , Created_(other.Created_)
{
    other.Created_ = false;
}

TCGroup::~TCGroup()
{
    if (Created_) {
        Destroy();
    }
}

void TCGroup::Create()
{
    EnsureExistance();
    Created_ = true;
}

void TCGroup::Destroy()
{
    LOG_INFO("Destroying cgroup %v", FullPath_);
    YCHECK(Created_);

#ifdef _linux_
    try {
        NFS::Remove(FullPath_);
    } catch (const std::exception& ex) {
        LOG_FATAL(ex, "Failed to destroy cgroup %Qv", FullPath_);
    }
#endif
    Created_ = false;
}

bool TCGroup::IsCreated() const
{
    return Created_;
}

////////////////////////////////////////////////////////////////////////////////

TCpuAccounting::TCpuAccounting(const Stroka& name)
    : TCGroup("cpuacct", name)
{ }

TCpuAccounting::TStatistics TCpuAccounting::GetStatistics() const
{
    TCpuAccounting::TStatistics result;
#ifdef _linux_
    try {
        auto path = NFS::CombinePaths(GetFullPath(), "cpuacct.stat");
        auto values = ReadAllValues(path);
        YCHECK(values.size() == 4);

        Stroka type[2];
        i64 jiffies[2];

        for (int i = 0; i < 2; ++i) {
            type[i] = values[2 * i];
            jiffies[i] = FromString<i64>(values[2 * i + 1]);
        }

        for (int i = 0; i < 2; ++i) {
            if (type[i] == "user") {
                result.UserTime = FromJiffies(jiffies[i]);
            } else if (type[i] == "system") {
                result.SystemTime = FromJiffies(jiffies[i]);
            }
        }
    } catch (const std::exception& ex) {
        LOG_FATAL(
            ex,
            "Failed to retreive CPU statistics from cgroup %Qv",
            GetFullPath());
    }
#endif
    return result;
}

void Serialize(const TCpuAccounting::TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("user").Value(static_cast<i64>(statistics.UserTime.MilliSeconds()))
            .Item("system").Value(static_cast<i64>(statistics.SystemTime.MilliSeconds()))
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TBlockIO::TBlockIO(const Stroka& name)
    : TCGroup("blkio", name)
{ }

// For more information about format of data
// read https://www.kernel.org/doc/Documentation/cgroups/blkio-controller.txt

TBlockIO::TStatistics TBlockIO::GetStatistics() const
{
    TBlockIO::TStatistics result;
#ifdef _linux_
        auto bytesStats = GetDetailedStatistics("blkio.io_service_bytes");
        for (const auto& item : bytesStats) {
            if (item.Type == "Read") {
                result.BytesRead += item.Value;
            } else if (item.Type == "Write") {
                result.BytesWritten += item.Value;
            }
        }

        auto ioStats = GetDetailedStatistics("blkio.io_serviced");
        for (const auto& item : ioStats) {
            if (item.Type == "Read") {
                result.IORead += item.Value;
            } else if (item.Type == "Write") {
                result.IOWrite += item.Value;
            }
        }
#endif
    return result;
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetIOServiceBytes() const
{
    return GetDetailedStatistics("blkio.io_service_bytes");
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetIOServiced() const
{
    return GetDetailedStatistics("blkio.io_serviced");
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetDetailedStatistics(const char* filename) const
{
    std::vector<TBlockIO::TStatisticsItem> result;
#ifdef _linux_
    try {
        auto path = NFS::CombinePaths(GetFullPath(), filename);
        auto values = ReadAllValues(path);

        int lineNumber = 0;
        while (3 * lineNumber + 2 < values.size()) {
            TStatisticsItem item;
            item.DeviceId = values[3 * lineNumber];
            item.Type = values[3 * lineNumber + 1];
            item.Value = FromString<i64>(values[3 * lineNumber + 2]);

            YCHECK(item.DeviceId.has_prefix("8:"));

            if (item.Type == "Read" || item.Type == "Write") {
                result.push_back(item);
            }
            ++lineNumber;
        }
    } catch (const std::exception& ex) {
        LOG_FATAL(
            ex,
            "Failed to retreive block io statistics from cgroup %Qv",
            GetFullPath());
    }
#endif
    return result;
}

void TBlockIO::ThrottleOperations(const Stroka& deviceId, i64 operations) const
{
    auto value = Format("%v %v", deviceId, operations);
    Append("blkio.throttle.read_iops_device", value);
    Append("blkio.throttle.write_iops_device", value);
}

void Serialize(const TBlockIO::TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("bytes_read").Value(statistics.BytesRead)
            .Item("bytes_written").Value(statistics.BytesWritten)
            .Item("io_read").Value(statistics.IORead)
            .Item("io_write").Value(statistics.IOWrite)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TMemory::TMemory(const Stroka& name)
    : TCGroup("memory", name)
{ }

TMemory::TStatistics TMemory::GetStatistics() const
{
    TMemory::TStatistics result;
#ifdef _linux_
     try {
        auto values = ReadAllValues(GetPath("memory.stat"));
        int lineNumber = 0;
        while (2 * lineNumber + 1 < values.size()) {
            const auto& type = values[2 * lineNumber];
            const auto value = FromString<i64>(values[2 * lineNumber + 1]);
            if (type == "rss") {
                result.Rss = value;
            }
            if (type == "mapped_file") {
                result.MappedFile = value;
            }
            ++lineNumber;
        }
    } catch (const std::exception& ex) {
        LOG_FATAL(
            ex,
            "Failed to retreive memory statistics from cgroup %Qv",
            GetFullPath());
    }
#endif
    return result;
}

void TMemory::SetLimitInBytes(i64 bytes) const
{
    Set("memory.limit_in_bytes", ToString(bytes));
}

void TMemory::ForceEmpty() const
{
    Set("memory.force_empty", "0");
}

void Serialize(const TMemory::TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("rss").Value(statistics.Rss)
            .Item("mapped_file").Value(statistics.MappedFile)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TFreezer::TFreezer(const Stroka& name)
    : TCGroup("freezer", name)
{ }

Stroka TFreezer::GetState() const
{
    return Get("freezer.state");
}

void TFreezer::Freeze() const
{
    Set("freezer.state", "FROZEN");
}

void TFreezer::Unfreeze() const
{
    Set("freezer.state", "THAWED");
}

////////////////////////////////////////////////////////////////////////////////

std::map<Stroka, Stroka> ParseProcessCGroups(const Stroka& str)
{
    std::map<Stroka, Stroka> result;

    yvector<Stroka> values;
    Split(str.data(), ":\n", values);
    for (size_t i = 0; i + 2 < values.size(); i += 3) {
        FromString<int>(values[i]);

        const Stroka& subsystemsSet = values[i + 1];
        const Stroka& name = values[i + 2];

        yvector<Stroka> subsystems;
        Split(subsystemsSet.data(), ",", subsystems);
        for (const auto& subsystem : subsystems) {
            if (!subsystem.has_prefix("name=")) {
                int start = 0;
                if (name.has_prefix("/")) {
                    start = 1;
                }
                result[subsystem] = name.substr(start);
            }
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCGroup
} // namespace NYT
