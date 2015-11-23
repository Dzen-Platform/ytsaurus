#pragma once

#include <core/profiling/profile_manager.h>

namespace NYT {
namespace NConcurrency {

///////////////////////////////////////////////////////////////////////////////

inline NProfiling::TTagIdList GetThreadTagIds(
    bool enableProfiling,
    const Stroka& threadName)
{
    using namespace NProfiling;
    TTagIdList result;
    if (enableProfiling) {
        auto* profilingManager = TProfileManager::Get();
        result.push_back(profilingManager->RegisterTag("thread", threadName));
    }
    return result;
}

inline NProfiling::TTagIdList GetBucketTagIds(
    bool enableProfiling,
    const Stroka& threadName,
    const Stroka& bucketName)
{
    using namespace NProfiling;
    TTagIdList result;
    if (enableProfiling) {
        auto* profilingManager = TProfileManager::Get();
        result.emplace_back(profilingManager->RegisterTag("thread", threadName));
        result.emplace_back(profilingManager->RegisterTag("bucket", bucketName));
    }
    return result;
}

inline std::vector<NProfiling::TTagIdList> GetBucketsTagIds(
    bool enableProfiling,
    const Stroka& threadName,
    const std::vector<Stroka>& bucketNames)
{
    using namespace NProfiling;
    std::vector<TTagIdList> result;
    for (const auto& bucketName : bucketNames) {
        result.emplace_back(GetBucketTagIds(enableProfiling, threadName, bucketName));
    }
    return result;
}

inline NProfiling::TTagIdList GetInvokerTagIds(const Stroka& invokerName)
{
    using namespace NProfiling;
    TTagIdList result;
    auto* profilingManager = TProfileManager::Get();
    result.push_back(profilingManager->RegisterTag("invoker", invokerName));
    return result;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT

