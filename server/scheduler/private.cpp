#include "private.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

const double ApproximateSizesBoostFactor = 1.3;
const double JobSizeBoostFactor = 2.0;

const Stroka RootPoolName = "<Root>";

const NLogging::TLogger SchedulerLogger("Scheduler");
const NLogging::TLogger OperationLogger("Operation");
const NProfiling::TProfiler SchedulerProfiler("/scheduler");

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

