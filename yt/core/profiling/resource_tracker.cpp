#include "resource_tracker.h"
#include "profile_manager.h"
#include "profiler.h"
#include "timing.h"

#include <yt/core/misc/fs.h>
#include <yt/core/misc/lfalloc_helpers.h>
#include <yt/core/misc/proc.h>

#include <yt/core/ypath/token.h>

#include <util/folder/filelist.h>

#include <util/stream/file.h>

#include <util/string/vector.h>

#ifdef RESOURCE_TRACKER_ENABLED
    #include <unistd.h>
#endif

namespace NYT {
namespace NProfiling {

using namespace NYPath;
using namespace NYTree;
using namespace NProfiling;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const TDuration UpdatePeriod = TDuration::Seconds(1);
static TProfiler Profiler("/resource_tracker");

////////////////////////////////////////////////////////////////////////////////

namespace {

i64 GetTicksPerSecond()
{
#ifdef RESOURCE_TRACKER_ENABLED
    return sysconf(_SC_CLK_TCK);
#else
    return -1;
#endif
}

} // namespace

// Please, refer to /proc documentation to know more about available information.
// http://www.kernel.org/doc/Documentation/filesystems/proc.txt

TResourceTracker::TResourceTracker(IInvokerPtr invoker)
    // CPU time is measured in jiffies; we need USER_HZ to convert them
    // to milliseconds and percentages.
    : TicksPerSecond(GetTicksPerSecond())
    , LastUpdateTime(TInstant::Now())
{
    PeriodicExecutor = New<TPeriodicExecutor>(
        invoker,
        BIND(&TResourceTracker::EnqueueUsage, Unretained(this)),
        UpdatePeriod);
}

void TResourceTracker::Start()
{
    PeriodicExecutor->Start();
}

void TResourceTracker::EnqueueUsage()
{
    EnqueueMemoryUsage();
    EnqueueCpuUsage();
}

void TResourceTracker::EnqueueCpuUsage()
{
    i64 timeDelta = TInstant::Now().MilliSeconds() - LastUpdateTime.MilliSeconds();
    if (timeDelta == 0)
        return;

    Stroka procPath("/proc/self/task");

    TDirsList dirsList;
    try {
        dirsList.Fill(procPath);
    } catch (const TIoException&) {
        // Ignore all IO exceptions.
        return;
    }

    std::unordered_map<Stroka, std::pair<i64, i64>> threadStats;

    for (int index = 0; index < dirsList.Size(); ++index) {
        auto threadStatPath = NFS::CombinePaths(procPath, dirsList.Next());
        auto cpuStatPath = NFS::CombinePaths(threadStatPath, "stat");

        std::vector<Stroka> fields;
        try {
            TIFStream cpuStatFile(cpuStatPath);
            fields = splitStroku(cpuStatFile.ReadLine(), " ");
        } catch (const TIoException&) {
            // Ignore all IO exceptions.
            continue;
        }

        // Get rid of parentheses in process title.
        YCHECK(fields[1].size() >= 2);

        auto threadName = fields[1].substr(1, fields[1].size() - 2);
        i64 userJiffies = FromString<i64>(fields[13]); // In jiffies
        i64 systemJiffies = FromString<i64>(fields[14]); // In jiffies

        auto it = threadStats.find(threadName);
        if (it == threadStats.end()) {
            threadStats.emplace(threadName, std::make_pair(userJiffies, systemJiffies));
        } else {
            it->second.first += userJiffies;
            it->second.second += systemJiffies;
        }
    }

    for (const auto& stat : threadStats)
    {
        const auto& threadName = stat.first;
        auto userJiffies = stat.second.first;
        auto systemJiffies = stat.second.second;

        auto it = ThreadNameToJiffies.find(threadName);
        if (it != ThreadNameToJiffies.end()) {
            auto& jiffies = it->second;
            i64 userCpuTime = (userJiffies - jiffies.PreviousUser) * 1000 / TicksPerSecond;
            i64 systemCpuTime = (systemJiffies - jiffies.PreviousSystem) * 1000 / TicksPerSecond;

            TTagIdList tagIds;
            tagIds.push_back(TProfileManager::Get()->RegisterTag("thread", threadName));

            Profiler.Enqueue("/user_cpu", 100 * userCpuTime / timeDelta, EMetricType::Gauge, tagIds);
            Profiler.Enqueue("/system_cpu", 100 * systemCpuTime / timeDelta, EMetricType::Gauge, tagIds);
        }

        {
            auto& jiffies = ThreadNameToJiffies[threadName];
            jiffies.PreviousUser = userJiffies;
            jiffies.PreviousSystem = systemJiffies;
        }
    }

    LastUpdateTime = TInstant::Now();
}

void TResourceTracker::EnqueueMemoryUsage()
{
    try {
        Profiler.Enqueue("/total/memory", GetProcessRss(), EMetricType::Gauge);
    } catch (const TIoException&) {
        // Ignore all IO exceptions.
        return;
    }
    EnqueueLFAllocCounters();
}

void TResourceTracker::EnqueueLFAllocCounters()
{
    using namespace NLFAlloc;

    Profiler.Enqueue("/lf_alloc/total/user_allocated", GetUserAllocated(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/mmapped", GetMmapped(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/mmapped_count", GetMmappedCount(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/munmapped", GetMunmapped(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/munmapped_count", GetMunmappedCount(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/system_allocated", GetSystemAllocated(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/system_deallocated", GetSystemFreed(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/small_blocks_allocated", GetSmallBlocksAllocated(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/small_blocks_deallocated", GetSmallBlocksFreed(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/large_blocks_allocated", GetLargeBlocksAllocated(), EMetricType::Counter);
    Profiler.Enqueue("/lf_alloc/total/large_blocks_deallocated", GetLargeBlocksFreed(), EMetricType::Counter);

    Profiler.Enqueue("/lf_alloc/current/system", GetCurrentSystem(), EMetricType::Gauge);
    Profiler.Enqueue("/lf_alloc/current/small_blocks", GetCurrentSmallBlocks(), EMetricType::Gauge);
    Profiler.Enqueue("/lf_alloc/current/large_blocks", GetCurrentLargeBlocks(), EMetricType::Gauge);

    auto mmapped = NLFAlloc::GetCurrentMmapped();
    Profiler.Enqueue("/lf_alloc/current/mmapped", mmapped, EMetricType::Gauge);

    auto mmappedCount = NLFAlloc::GetCurrentMmappedCount();
    Profiler.Enqueue("/lf_alloc/current/mmapped_count", mmappedCount, EMetricType::Gauge);

    auto used = NLFAlloc::GetCurrentUsed();
    Profiler.Enqueue("/lf_alloc/current/used", used, EMetricType::Gauge);
    Profiler.Enqueue("/lf_alloc/current/locked", mmapped - used, EMetricType::Gauge);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
