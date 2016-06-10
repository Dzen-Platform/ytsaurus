#pragma once

#include "public.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<ISchedulerStrategy> CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
