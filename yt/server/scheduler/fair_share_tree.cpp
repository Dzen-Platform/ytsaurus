#include "fair_share_tree.h"

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/profile_manager.h>

#include <yt/core/misc/finally.h>

#include <yt/core/profiling/scoped_timer.h>

namespace NYT {
namespace NScheduler {

using namespace NConcurrency;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////

static const double RatioComputationPrecision = std::numeric_limits<double>::epsilon();
static const double RatioComparisonPrecision = sqrt(RatioComputationPrecision);

////////////////////////////////////////////////////////////////////

TJobResources ToJobResources(const TResourceLimitsConfigPtr& config, TJobResources defaultValue)
{
    if (config->UserSlots) {
        defaultValue.SetUserSlots(*config->UserSlots);
    }
    if (config->Cpu) {
        defaultValue.SetCpu(*config->Cpu);
    }
    if (config->Network) {
        defaultValue.SetNetwork(*config->Network);
    }
    if (config->Memory) {
        defaultValue.SetMemory(*config->Memory);
    }
    return defaultValue;
}

////////////////////////////////////////////////////////////////////

TFairShareContext::TFairShareContext(
    const ISchedulingContextPtr& schedulingContext,
    int treeSize,
    const std::vector<TSchedulingTagFilter>& registeredSchedulingTagFilters)
    : SchedulingContext(schedulingContext)
    , DynamicAttributesList(treeSize)
{
    CanSchedule.reserve(registeredSchedulingTagFilters.size());
    for (const auto& filter : registeredSchedulingTagFilters) {
        CanSchedule.push_back(SchedulingContext->CanSchedule(filter));
    }
}

TDynamicAttributes& TFairShareContext::DynamicAttributes(TSchedulerElement* element)
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

const TDynamicAttributes& TFairShareContext::DynamicAttributes(TSchedulerElement* element) const
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

////////////////////////////////////////////////////////////////////

TSchedulerElementFixedState::TSchedulerElementFixedState(
    ISchedulerStrategyHost* host,
    const TFairShareStrategyConfigPtr& strategyConfig)
    : ResourceDemand_(ZeroJobResources())
    , ResourceLimits_(InfiniteJobResources())
    , MaxPossibleResourceUsage_(ZeroJobResources())
    , Host_(host)
    , StrategyConfig_(strategyConfig)
    , TotalResourceLimits_(host->GetMainNodesResourceLimits())
{ }

////////////////////////////////////////////////////////////////////

TSchedulerElementSharedState::TSchedulerElementSharedState()
    : ResourceUsage_(ZeroJobResources())
{ }

TJobResources TSchedulerElementSharedState::GetResourceUsage()
{
    TReaderGuard guard(ResourceUsageLock_);

    return ResourceUsage_;
}

void TSchedulerElementSharedState::IncreaseResourceUsage(const TJobResources& delta)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsage_ += delta;
}

double TSchedulerElementSharedState::GetResourceUsageRatio(
    EResourceType dominantResource,
    double dominantResourceLimit)
{
    TReaderGuard guard(ResourceUsageLock_);

    if (dominantResourceLimit == 0) {
        return 0.0;
    }
    return GetResource(ResourceUsage_, dominantResource) / dominantResourceLimit;
}

////////////////////////////////////////////////////////////////////

int TSchedulerElement::EnumerateNodes(int startIndex)
{
    YCHECK(!Cloned_);

    TreeIndex_ = startIndex++;
    return startIndex;
}

void TSchedulerElement::UpdateStrategyConfig(const TFairShareStrategyConfigPtr& config)
{
    StrategyConfig_ = config;
}

void TSchedulerElement::Update(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    UpdateBottomUp(dynamicAttributesList);
    UpdateTopDown(dynamicAttributesList);
}

void TSchedulerElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TotalResourceLimits_ = GetHost()->GetMainNodesResourceLimits();
    UpdateAttributes();
    dynamicAttributesList[GetTreeIndex()].Active = true;
    UpdateDynamicAttributes(dynamicAttributesList);
}

void TSchedulerElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);
}

void TSchedulerElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(IsActive(dynamicAttributesList));
    dynamicAttributesList[GetTreeIndex()].SatisfactionRatio = ComputeLocalSatisfactionRatio();
    dynamicAttributesList[GetTreeIndex()].Active = IsAlive();
}

void TSchedulerElement::PrescheduleJob(TFairShareContext& context, bool /*starvingOnly*/, bool /*aggressiveStarvationEnabled*/)
{
    UpdateDynamicAttributes(context.DynamicAttributesList);
}

void TSchedulerElement::UpdateAttributes()
{
    YCHECK(!Cloned_);

    // Choose dominant resource types, compute max share ratios, compute demand ratios.
    const auto& demand = ResourceDemand();
    auto usage = GetResourceUsage();

    auto maxPossibleResourceUsage = Min(TotalResourceLimits_, MaxPossibleResourceUsage_);

    if (usage == ZeroJobResources()) {
        Attributes_.DominantResource = GetDominantResource(demand, TotalResourceLimits_);
    } else {
        Attributes_.DominantResource = GetDominantResource(usage, TotalResourceLimits_);
    }

    Attributes_.DominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);;

    auto dominantDemand = GetResource(demand, Attributes_.DominantResource);
    Attributes_.DemandRatio =
        Attributes_.DominantLimit == 0 ? 1.0 : dominantDemand / Attributes_.DominantLimit;

    auto possibleUsage = usage + ComputePossibleResourceUsage(maxPossibleResourceUsage - usage);
    double possibleUsageRatio = GetDominantResourceUsage(possibleUsage, TotalResourceLimits_);

    Attributes_.MaxPossibleUsageRatio = std::min(
        possibleUsageRatio,
        GetMaxShareRatio());
}

const TSchedulingTagFilter& TSchedulerElement::GetSchedulingTagFilter() const
{
    return EmptySchedulingTagFilter;
}

bool TSchedulerElement::IsActive(const TDynamicAttributesList& dynamicAttributesList) const
{
    return dynamicAttributesList[GetTreeIndex()].Active;
}

TCompositeSchedulerElement* TSchedulerElement::GetParent() const
{
    return Parent_;
}

void TSchedulerElement::SetParent(TCompositeSchedulerElement* parent)
{
    YCHECK(!Cloned_);

    Parent_ = parent;
}

TInstant TSchedulerElement::GetStartTime() const
{
    return StartTime_;
}

int TSchedulerElement::GetPendingJobCount() const
{
    return PendingJobCount_;
}

ESchedulableStatus TSchedulerElement::GetStatus() const
{
    return ESchedulableStatus::Normal;
}

bool TSchedulerElement::GetStarving() const
{
    return Starving_;
}

void TSchedulerElement::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    Starving_ = starving;
}

TJobResources TSchedulerElement::GetResourceUsage() const
{
    auto resourceUsage = SharedState_->GetResourceUsage();
    if (resourceUsage.GetUserSlots() > 0 && resourceUsage.GetMemory() == 0) {
        LOG_WARNING("Found usage of schedulable element %Qv with non-zero user slots and zero memory", GetId());
    }
    return resourceUsage;
}

double TSchedulerElement::GetResourceUsageRatio() const
{
    return SharedState_->GetResourceUsageRatio(
        Attributes_.DominantResource,
        Attributes_.DominantLimit);
}

void TSchedulerElement::IncreaseLocalResourceUsage(const TJobResources& delta)
{
    SharedState_->IncreaseResourceUsage(delta);
}

TSchedulerElement::TSchedulerElement(
    ISchedulerStrategyHost* host,
    const TFairShareStrategyConfigPtr& strategyConfig)
    : TSchedulerElementFixedState(host, strategyConfig)
    , SharedState_(New<TSchedulerElementSharedState>())
{ }

TSchedulerElement::TSchedulerElement(
    const TSchedulerElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElementFixedState(other)
    , SharedState_(other.SharedState_)
{
    Parent_ = clonedParent;
    Cloned_ = true;
}

ISchedulerStrategyHost* TSchedulerElement::GetHost() const
{
    YCHECK(!Cloned_);

    return Host_;
}

double TSchedulerElement::ComputeLocalSatisfactionRatio() const
{
    double minShareRatio = Attributes_.AdjustedMinShareRatio;
    double fairShareRatio = Attributes_.FairShareRatio;
    double usageRatio = GetResourceUsageRatio();

    // Check for corner cases.
    if (fairShareRatio < RatioComputationPrecision) {
        return std::numeric_limits<double>::max();
    }

    if (minShareRatio > RatioComputationPrecision && usageRatio < minShareRatio) {
        // Needy element, negative satisfaction.
        return usageRatio / minShareRatio - 1.0;
    } else {
        // Regular element, positive satisfaction.
        return usageRatio / fairShareRatio;
    }
}

ESchedulableStatus TSchedulerElement::GetStatus(double defaultTolerance) const
{
    double usageRatio = GetResourceUsageRatio();
    double demandRatio = Attributes_.DemandRatio;

    double tolerance =
        demandRatio < Attributes_.FairShareRatio + RatioComparisonPrecision
        ? 1.0
        : defaultTolerance;

    if (usageRatio > Attributes_.FairShareRatio * tolerance - RatioComparisonPrecision) {
        return ESchedulableStatus::Normal;
    }

    return usageRatio < Attributes_.AdjustedMinShareRatio
           ? ESchedulableStatus::BelowMinShare
           : ESchedulableStatus::BelowFairShare;
}

void TSchedulerElement::CheckForStarvationImpl(
    TDuration minSharePreemptionTimeout,
    TDuration fairSharePreemptionTimeout,
    TInstant now)
{
    YCHECK(!Cloned_);

    auto status = GetStatus();
    switch (status) {
        case ESchedulableStatus::BelowMinShare:
            if (!BelowFairShareSince_) {
                BelowFairShareSince_ = now;
            } else if (BelowFairShareSince_.Get() < now - minSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::BelowFairShare:
            if (!BelowFairShareSince_) {
                BelowFairShareSince_ = now;
            } else if (BelowFairShareSince_.Get() < now - fairSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::Normal:
            BelowFairShareSince_ = Null;
            SetStarving(false);
            break;

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////

TCompositeSchedulerElement::TCompositeSchedulerElement(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig,
    const Stroka& profilingName)
    : TSchedulerElement(host, strategyConfig)
    , ProfilingTag_(NProfiling::TProfileManager::Get()->RegisterTag("pool", profilingName))
{ }

TCompositeSchedulerElement::TCompositeSchedulerElement(
    const TCompositeSchedulerElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElement(other, clonedParent)
    , TCompositeSchedulerElementFixedState(other)
    , ProfilingTag_(other.ProfilingTag_)
{
    auto cloneChildren = [&] (
        const std::vector<TSchedulerElementPtr>& list,
        yhash_map<TSchedulerElementPtr, int>* clonedMap,
        std::vector<TSchedulerElementPtr>* clonedList)
    {
        for (const auto& child : list) {
            auto childClone = child->Clone(this);
            clonedList->push_back(childClone);
            YCHECK(clonedMap->emplace(childClone, clonedList->size() - 1).second);
        }
    };
    cloneChildren(other.EnabledChildren_, &EnabledChildToIndex_, &EnabledChildren_);
    cloneChildren(other.DisabledChildren_, &DisabledChildToIndex_, &DisabledChildren_);
}

int TCompositeSchedulerElement::EnumerateNodes(int startIndex)
{
    YCHECK(!Cloned_);

    startIndex = TSchedulerElement::EnumerateNodes(startIndex);
    for (const auto& child : EnabledChildren_) {
        startIndex = child->EnumerateNodes(startIndex);
    }
    return startIndex;
}

void TCompositeSchedulerElement::UpdateStrategyConfig(const TFairShareStrategyConfigPtr& config)
{
    TSchedulerElement::UpdateStrategyConfig(config);

    auto updateChildrenConfig = [&config] (TChildList& list) {
        for (const auto& child : list) {
            child->UpdateStrategyConfig(config);
        }
    };

    updateChildrenConfig(EnabledChildren_);
    updateChildrenConfig(DisabledChildren_);
}

void TCompositeSchedulerElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    Attributes_.BestAllocationRatio = 0.0;
    PendingJobCount_ = 0;
    ResourceDemand_ = ZeroJobResources();
    auto maxPossibleChildrenResourceUsage_ = ZeroJobResources();
    for (const auto& child : EnabledChildren_) {
        child->UpdateBottomUp(dynamicAttributesList);

        Attributes_.BestAllocationRatio = std::max(
            Attributes_.BestAllocationRatio,
            child->Attributes().BestAllocationRatio);

        PendingJobCount_ += child->GetPendingJobCount();
        ResourceDemand_ += child->ResourceDemand();
        maxPossibleChildrenResourceUsage_ += child->MaxPossibleResourceUsage();
    }
    MaxPossibleResourceUsage_ = Min(maxPossibleChildrenResourceUsage_, ResourceLimits_);
    TSchedulerElement::UpdateBottomUp(dynamicAttributesList);
}

void TCompositeSchedulerElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    switch (Mode_) {
        case ESchedulingMode::Fifo:
            // Easy case -- the first child get everything, others get none.
            UpdateFifo(dynamicAttributesList);
            break;

        case ESchedulingMode::FairShare:
            // Hard case -- compute fair shares using fit factor.
            UpdateFairShare(dynamicAttributesList);
            break;

        default:
            Y_UNREACHABLE();
    }

    UpdatePreemptionSettingsLimits();

    // Propagate updates to children.
    for (const auto& child : EnabledChildren_) {
        UpdateChildPreemptionSettings(child);
        child->UpdateTopDown(dynamicAttributesList);
    }
}

TJobResources TCompositeSchedulerElement::ComputePossibleResourceUsage(TJobResources limit) const
{
    limit = Min(limit, MaxPossibleResourceUsage() - GetResourceUsage());
    auto additionalUsage = ZeroJobResources();
    for (const auto& child : EnabledChildren_) {
        auto childUsage = child->ComputePossibleResourceUsage(limit);
        limit -= childUsage;
        additionalUsage += childUsage;
    }
    return additionalUsage;
}

double TCompositeSchedulerElement::GetFairShareStarvationToleranceLimit() const
{
    return 1.0;
}

TDuration TCompositeSchedulerElement::GetMinSharePreemptionTimeoutLimit() const
{
    return TDuration::Zero();
}

TDuration TCompositeSchedulerElement::GetFairSharePreemptionTimeoutLimit() const
{
    return TDuration::Zero();
}

void TCompositeSchedulerElement::UpdatePreemptionSettingsLimits()
{
    YCHECK(!Cloned_);

    if (Parent_) {
        AdjustedFairShareStarvationToleranceLimit_ = std::min(
            GetFairShareStarvationToleranceLimit(),
            Parent_->AdjustedFairShareStarvationToleranceLimit());

        AdjustedMinSharePreemptionTimeoutLimit_ = std::max(
            GetMinSharePreemptionTimeoutLimit(),
            Parent_->AdjustedMinSharePreemptionTimeoutLimit());

        AdjustedFairSharePreemptionTimeoutLimit_ = std::max(
            GetFairSharePreemptionTimeoutLimit(),
            Parent_->AdjustedFairSharePreemptionTimeoutLimit());
    }
}

void TCompositeSchedulerElement::UpdateChildPreemptionSettings(const TSchedulerElementPtr& child)
{
    YCHECK(!Cloned_);

    auto& childAttributes = child->Attributes();

    childAttributes.AdjustedFairShareStarvationTolerance = std::min(
        child->GetFairShareStarvationTolerance(),
        AdjustedFairShareStarvationToleranceLimit_);

    childAttributes.AdjustedMinSharePreemptionTimeout = std::max(
        child->GetMinSharePreemptionTimeout(),
        AdjustedMinSharePreemptionTimeoutLimit_);

    childAttributes.AdjustedFairSharePreemptionTimeout = std::max(
        child->GetFairSharePreemptionTimeout(),
        AdjustedFairSharePreemptionTimeoutLimit_);
}

void TCompositeSchedulerElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(IsActive(dynamicAttributesList));
    auto& attributes = dynamicAttributesList[GetTreeIndex()];

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    // Compute local satisfaction ratio.
    attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio();
    // Adjust satisfaction ratio using children.
    // Declare the element passive if all children are passive.
    attributes.Active = false;
    attributes.BestLeafDescendant = nullptr;

    while (auto bestChild = GetBestActiveChild(dynamicAttributesList)) {
        const auto& bestChildAttributes = dynamicAttributesList[bestChild->GetTreeIndex()];
        auto childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        if (!childBestLeafDescendant->IsAlive()) {
            bestChild->UpdateDynamicAttributes(dynamicAttributesList);
            if (!bestChildAttributes.Active) {
                continue;
            }
            childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        }

        attributes.SatisfactionRatio = std::min(
            attributes.SatisfactionRatio,
            bestChildAttributes.SatisfactionRatio);

        attributes.BestLeafDescendant = childBestLeafDescendant;
        attributes.Active = true;
        break;
    }
}

void TCompositeSchedulerElement::BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap)
{
    for (const auto& child : EnabledChildren_) {
        child->BuildOperationToElementMapping(operationElementByIdMap);
    }
}

void TCompositeSchedulerElement::PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled)
{
    auto& attributes = context.DynamicAttributes(this);

    attributes.Active = true;

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    if (StrategyConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context.CanSchedule[SchedulingTagFilterIndex_])
    {
        attributes.Active = false;
        return;
    }

    aggressiveStarvationEnabled = aggressiveStarvationEnabled || IsAggressiveStarvationEnabled();
    if (Starving_ && aggressiveStarvationEnabled) {
        context.HasAggressivelyStarvingNodes = true;
    }

    // If pool is starving, any child will do.
    bool starvingOnlyChildren = Starving_ ? false : starvingOnly;
    for (const auto& child : EnabledChildren_) {
        child->PrescheduleJob(context, starvingOnlyChildren, aggressiveStarvationEnabled);
    }

    TSchedulerElement::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);
}

bool TCompositeSchedulerElement::ScheduleJob(TFairShareContext& context)
{
    auto& attributes = context.DynamicAttributes(this);
    if (!attributes.Active) {
        return false;
    }

    auto bestLeafDescendant = attributes.BestLeafDescendant;
    if (!bestLeafDescendant->IsAlive()) {
        UpdateDynamicAttributes(context.DynamicAttributesList);
        if (!attributes.Active) {
            return false;
        }
        bestLeafDescendant = attributes.BestLeafDescendant;
    }

    // NB: Ignore the child's result.
    bestLeafDescendant->ScheduleJob(context);
    return true;
}

void TCompositeSchedulerElement::IncreaseResourceUsage(const TJobResources& delta)
{
    auto* currentElement = this;
    while (currentElement) {
        currentElement->IncreaseLocalResourceUsage(delta);
        currentElement = currentElement->GetParent();
    }
}

bool TCompositeSchedulerElement::IsRoot() const
{
    return false;
}

bool TCompositeSchedulerElement::IsExplicit() const
{
    return false;
}

bool TCompositeSchedulerElement::IsAggressiveStarvationEnabled() const
{
    return false;
}

void TCompositeSchedulerElement::AddChild(const TSchedulerElementPtr& child, bool enabled)
{
    YCHECK(!Cloned_);

    auto& map = enabled ? EnabledChildToIndex_ : DisabledChildToIndex_;
    auto& list = enabled ? EnabledChildren_ : DisabledChildren_;
    AddChild(&map, &list, child);
}

void TCompositeSchedulerElement::EnableChild(const TSchedulerElementPtr& child)
{
    YCHECK(!Cloned_);

    RemoveChild(&DisabledChildToIndex_, &DisabledChildren_, child);
    AddChild(&EnabledChildToIndex_, &EnabledChildren_, child);
}

void TCompositeSchedulerElement::RemoveChild(const TSchedulerElementPtr& child)
{
    YCHECK(!Cloned_);

    bool enabled = ContainsChild(EnabledChildToIndex_, child);
    auto& map = enabled ? EnabledChildToIndex_ : DisabledChildToIndex_;
    auto& list = enabled ? EnabledChildren_ : DisabledChildren_;
    RemoveChild(&map, &list, child);
}

bool TCompositeSchedulerElement::IsEmpty() const
{
    return EnabledChildren_.empty() && DisabledChildren_.empty();
}

ESchedulingMode TCompositeSchedulerElement::GetMode() const
{
    return Mode_;
}

void TCompositeSchedulerElement::SetMode(ESchedulingMode mode)
{
    Mode_ = mode;
}

NProfiling::TTagId TCompositeSchedulerElement::GetProfilingTag() const
{
    return ProfilingTag_;
}

// Given a non-descending continuous |f|, |f(0) = 0|, and a scalar |a|,
// computes |x \in [0,1]| s.t. |f(x) = a|.
// If |f(1) <= a| then still returns 1.
template <class F>
static double BinarySearch(const F& f, double a)
{
    if (f(1) <= a) {
        return 1.0;
    }

    double lo = 0.0;
    double hi = 1.0;
    while (hi - lo > RatioComputationPrecision) {
        double x = (lo + hi) / 2.0;
        if (f(x) < a) {
            lo = x;
        } else {
            hi = x;
        }
    }
    return (lo + hi) / 2.0;
}

template <class TGetter, class TSetter>
void TCompositeSchedulerElement::ComputeByFitting(
    const TGetter& getter,
    const TSetter& setter,
    double sum)
{
    auto getSum = [&] (double fitFactor) -> double {
        double sum = 0.0;
        for (const auto& child : EnabledChildren_) {
            sum += getter(fitFactor, child);
        }
        return sum;
    };

    // Run binary search to compute fit factor.
    double fitFactor = BinarySearch(getSum, sum);

    // Compute actual min shares from fit factor.
    for (const auto& child : EnabledChildren_) {
        double value = getter(fitFactor, child);
        setter(child, value);
    }
}

void TCompositeSchedulerElement::UpdateFifo(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    auto children = EnabledChildren_;
    std::sort(children.begin(), children.end(), BIND(&TCompositeSchedulerElement::HasHigherPriorityInFifoMode, MakeStrong(this)));

    int index = 0;
    for (const auto& child : children) {
        auto& childAttributes = child->Attributes();
        childAttributes.AdjustedMinShareRatio = 0.0;
        childAttributes.FairShareRatio = 0.0;
        childAttributes.FifoIndex = index;
        ++index;
    }
}

void TCompositeSchedulerElement::UpdateFairShare(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    UpdateFairShareAlerts_.clear();

    // Compute min shares sum and min weight.
    double minShareRatioSum = 0.0;
    double minWeight = 1.0;
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        auto minShareRatio = child->GetMinShareRatio();
        minShareRatioSum += minShareRatio;
        childAttributes.RecursiveMinShareRatio = Attributes_.RecursiveMinShareRatio * minShareRatio;

        if (minShareRatio > 0 && Attributes_.RecursiveMinShareRatio == 0) {
            UpdateFairShareAlerts_.emplace_back(
                "Min share ratio setting for %Qv has no effect "
                "because min share ratio of parent pool %Qv is zero",
                child->GetId(),
                GetId());
        }

        if (child->GetWeight() > RatioComputationPrecision) {
            minWeight = std::min(minWeight, child->GetWeight());
        }
    }

    // If min share sum is larger than one, adjust all children min shares to sum up to one.
    if (minShareRatioSum > 1.0) {
        UpdateFairShareAlerts_.emplace_back(
            "Total min share ratio of children of %Qv is too large: %v > 1",
            GetId(),
            minShareRatioSum);

        double fitFactor = 1.0 / minShareRatioSum;
        for (const auto& child : EnabledChildren_) {
            auto& childAttributes = child->Attributes();
            childAttributes.RecursiveMinShareRatio *= fitFactor;
        }
    }

    minShareRatioSum = 0.0;
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        childAttributes.AdjustedMinShareRatio = std::max(
            childAttributes.RecursiveMinShareRatio,
            GetMaxResourceRatio(child->GetMinShareResources(), TotalResourceLimits_));
        minShareRatioSum += childAttributes.AdjustedMinShareRatio;
    }

    if (minShareRatioSum > Attributes_.GuaranteedResourcesRatio) {
        UpdateFairShareAlerts_.emplace_back(
            "Impossible to satisfy resources guarantees for children of %Qv, "
            "given out resources share is greater than guaranteed resources share: %v > %v",
            GetId(),
            minShareRatioSum,
            Attributes_.GuaranteedResourcesRatio);

        double fitFactor = Attributes_.GuaranteedResourcesRatio / minShareRatioSum;
        for (const auto& child : EnabledChildren_) {
            auto& childAttributes = child->Attributes();
            childAttributes.AdjustedMinShareRatio *= fitFactor;
        }
    }

    // Compute fair shares.
    ComputeByFitting(
        [&] (double fitFactor, const TSchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.AdjustedMinShareRatio);
            // Never give more than can be used.
            result = std::min(result, childAttributes.MaxPossibleUsageRatio);
            // Never give more than we can allocate.
            result = std::min(result, childAttributes.BestAllocationRatio);
            return result;
        },
        [&] (const TSchedulerElementPtr& child, double value) {
            auto& attributes = child->Attributes();
            attributes.FairShareRatio = value;
        },
        Attributes_.FairShareRatio);

    // Compute guaranteed shares.
    ComputeByFitting(
        [&] (double fitFactor, const TSchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.AdjustedMinShareRatio);
            return result;
        },
        [&] (const TSchedulerElementPtr& child, double value) {
            auto& attributes = child->Attributes();
            attributes.GuaranteedResourcesRatio = value;
        },
        Attributes_.GuaranteedResourcesRatio);

    // Trim adjusted min share ratio with demand ratio.
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        double result = childAttributes.AdjustedMinShareRatio;
        // Never give more than can be used.
        result = std::min(result, childAttributes.MaxPossibleUsageRatio);
        // Never give more than we can allocate.
        result = std::min(result, childAttributes.BestAllocationRatio);
        childAttributes.AdjustedMinShareRatio = result;
    }
}

TSchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChild(const TDynamicAttributesList& dynamicAttributesList) const
{
    switch (Mode_) {
        case ESchedulingMode::Fifo:
            return GetBestActiveChildFifo(dynamicAttributesList);
        case ESchedulingMode::FairShare:
            return GetBestActiveChildFairShare(dynamicAttributesList);
        default:
            Y_UNREACHABLE();
    }
}

TSchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const
{
    TSchedulerElement* bestChild = nullptr;
    for (const auto& child : EnabledChildren_) {
        if (child->IsActive(dynamicAttributesList)) {
            if (bestChild && HasHigherPriorityInFifoMode(bestChild, child)) {
                continue;
            }

            bestChild = child.Get();
        }
    }
    return bestChild;
}

TSchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const
{
    TSchedulerElement* bestChild = nullptr;
    double bestChildSatisfactionRatio = std::numeric_limits<double>::max();
    for (const auto& child : EnabledChildren_) {
        if (child->IsActive(dynamicAttributesList)) {
            double childSatisfactionRatio = dynamicAttributesList[child->GetTreeIndex()].SatisfactionRatio;
            if (!bestChild || childSatisfactionRatio < bestChildSatisfactionRatio) {
                bestChild = child.Get();
                bestChildSatisfactionRatio = childSatisfactionRatio;
            }
        }
    }
    return bestChild;
}


void TCompositeSchedulerElement::AddChild(
    TChildMap* map,
    TChildList* list,
    const TSchedulerElementPtr& child)
{
    list->push_back(child);
    YCHECK(map->emplace(child, list->size() - 1).second);
}

void TCompositeSchedulerElement::RemoveChild(
    TChildMap* map,
    TChildList* list,
    const TSchedulerElementPtr& child)
{
    auto it = map->find(child);
    YCHECK(it != map->end());
    if (child == list->back()) {
        list->pop_back();
    } else {
        int index = it->second;
        std::swap((*list)[index], list->back());
        list->pop_back();
        (*map)[(*list)[index]] = index;
    }
    map->erase(it);
}

bool TCompositeSchedulerElement::ContainsChild(
    const TChildMap& map,
    const TSchedulerElementPtr& child)
{
    return map.find(child) != map.end();
}

bool TCompositeSchedulerElement::HasHigherPriorityInFifoMode(const TSchedulerElementPtr& lhs, const TSchedulerElementPtr& rhs) const
{
    for (auto parameter : FifoSortParameters_) {
        switch (parameter) {
            case EFifoSortParameter::Weight:
                if (lhs->GetWeight() != rhs->GetWeight()) {
                    return lhs->GetWeight() > rhs->GetWeight();
                }
                break;
            case EFifoSortParameter::StartTime: {
                const auto& lhsStartTime = lhs->GetStartTime();
                const auto& rhsStartTime = rhs->GetStartTime();
                if (lhsStartTime != rhsStartTime) {
                    return lhsStartTime < rhsStartTime;
                }
                break;
            }
            case EFifoSortParameter::PendingJobCount: {
                int lhsPendingJobCount = lhs->GetPendingJobCount();
                int rhsPendingJobCount = rhs->GetPendingJobCount();
                if (lhsPendingJobCount != rhsPendingJobCount) {
                    return lhsPendingJobCount < rhsPendingJobCount;
                }
                break;
            }
            default:
                Y_UNREACHABLE();
        }
    }
    return false;
}

////////////////////////////////////////////////////////////////////

TPoolFixedState::TPoolFixedState(const Stroka& id)
    : Id_(id)
{ }

////////////////////////////////////////////////////////////////////

TPool::TPool(
    ISchedulerStrategyHost* host,
    const Stroka& id,
    TFairShareStrategyConfigPtr strategyConfig)
    : TCompositeSchedulerElement(host, strategyConfig, id)
    , TPoolFixedState(id)
{
    SetDefaultConfig();
}

TPool::TPool(const TPool& other, TCompositeSchedulerElement* clonedParent)
    : TCompositeSchedulerElement(other, clonedParent)
    , TPoolFixedState(other)
    , Config_(other.Config_)
{ }

bool TPool::IsDefaultConfigured() const
{
    return DefaultConfigured_;
}

TPoolConfigPtr TPool::GetConfig()
{
    return Config_;
}

void TPool::SetConfig(TPoolConfigPtr config)
{
    YCHECK(!Cloned_);

    DoSetConfig(config);
    DefaultConfigured_ = false;
    SchedulingTagFilter_ = TSchedulingTagFilter(config->SchedulingTagFilter);
}

void TPool::SetDefaultConfig()
{
    YCHECK(!Cloned_);

    DoSetConfig(New<TPoolConfig>());
    DefaultConfigured_ = true;
    SchedulingTagFilter_ = EmptySchedulingTagFilter;
}

bool TPool::IsExplicit() const
{
    // NB: This is no coincidence.
    return !DefaultConfigured_;
}

bool TPool::IsAggressiveStarvationEnabled() const
{
    return Config_->EnableAggressiveStarvation;
}

Stroka TPool::GetId() const
{
    return Id_;
}

double TPool::GetWeight() const
{
    return Config_->Weight;
}

double TPool::GetMinShareRatio() const
{
    return Config_->MinShareRatio;
}

TJobResources TPool::GetMinShareResources() const
{
    return ToJobResources(Config_->MinShareResources, ZeroJobResources());
}

double TPool::GetMaxShareRatio() const
{
    return Config_->MaxShareRatio;
}

ESchedulableStatus TPool::GetStatus() const
{
    return TSchedulerElement::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

double TPool::GetFairShareStarvationTolerance() const
{
    return Config_->FairShareStarvationTolerance.Get(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TPool::GetMinSharePreemptionTimeout() const
{
    return Config_->MinSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TPool::GetFairSharePreemptionTimeout() const
{
    return Config_->FairSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

double TPool::GetFairShareStarvationToleranceLimit() const
{
    return Config_->FairShareStarvationToleranceLimit.Get(StrategyConfig_->FairShareStarvationToleranceLimit);
}

TDuration TPool::GetMinSharePreemptionTimeoutLimit() const
{
    return Config_->MinSharePreemptionTimeoutLimit.Get(StrategyConfig_->MinSharePreemptionTimeoutLimit);
}

TDuration TPool::GetFairSharePreemptionTimeoutLimit() const
{
    return Config_->FairSharePreemptionTimeoutLimit.Get(StrategyConfig_->FairSharePreemptionTimeoutLimit);
}

void TPool::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    if (starving && !GetStarving()) {
        TSchedulerElement::SetStarving(true);
        LOG_INFO("Pool is now starving (PoolId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElement::SetStarving(false);
        LOG_INFO("Pool is no longer starving (PoolId: %v)",
            GetId());
    }
}

void TPool::CheckForStarvation(TInstant now)
{
    YCHECK(!Cloned_);

    TSchedulerElement::CheckForStarvationImpl(
        Attributes_.AdjustedMinSharePreemptionTimeout,
        Attributes_.AdjustedFairSharePreemptionTimeout,
        now);
}

const TSchedulingTagFilter& TPool::GetSchedulingTagFilter() const
{
    return SchedulingTagFilter_;
}

void TPool::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    ResourceLimits_ = ComputeResourceLimits();
    TCompositeSchedulerElement::UpdateBottomUp(dynamicAttributesList);
}

int TPool::GetMaxRunningOperationCount() const
{
    return Config_->MaxRunningOperationCount.Get(StrategyConfig_->MaxRunningOperationCountPerPool);
}

int TPool::GetMaxOperationCount() const
{
    return Config_->MaxOperationCount.Get(StrategyConfig_->MaxOperationCountPerPool);
}

bool TPool::AreImmediateOperationsFobidden() const
{
    return Config_->ForbidImmediateOperations;
}

TSchedulerElementPtr TPool::Clone(TCompositeSchedulerElement* clonedParent)
{
    return New<TPool>(*this, clonedParent);
}

void TPool::DoSetConfig(TPoolConfigPtr newConfig)
{
    YCHECK(!Cloned_);

    Config_ = newConfig;
    FifoSortParameters_ = Config_->FifoSortParameters;
    Mode_ = Config_->Mode;
}

TJobResources TPool::ComputeResourceLimits() const
{
    auto resourceLimits = GetHost()->GetResourceLimits(GetSchedulingTagFilter()) * Config_->MaxShareRatio;
    auto perTypeLimits = ToJobResources(Config_->ResourceLimits, InfiniteJobResources());
    return Min(resourceLimits, perTypeLimits);
}

////////////////////////////////////////////////////////////////////

TOperationElementFixedState::TOperationElementFixedState(TOperationPtr operation)
    : Controller_(operation->GetController())
    , OperationId_(operation->GetId())
    , Schedulable_(operation->IsSchedulable())
    , Operation_(operation.Get())
{ }

////////////////////////////////////////////////////////////////////

TOperationElementSharedState::TOperationElementSharedState()
    : NonpreemptableResourceUsage_(ZeroJobResources())
    , AggressivelyPreemptableResourceUsage_(ZeroJobResources())
{ }

TJobResources TOperationElementSharedState::Finalize()
{
    TWriterGuard guard(JobPropertiesMapLock_);

    YCHECK(!Finalized_);
    Finalized_ = true;

    auto totalResourceUsage = ZeroJobResources();
    for (const auto& pair : JobPropertiesMap_) {
        totalResourceUsage += pair.second.ResourceUsage;
    }
    return totalResourceUsage;
}

TJobResources TOperationElementSharedState::IncreaseJobResourceUsage(
    const TJobId& jobId,
    const TJobResources& resourcesDelta)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (Finalized_) {
        return ZeroJobResources();
    }

    IncreaseJobResourceUsage(JobPropertiesMap_.at(jobId), resourcesDelta);
    return resourcesDelta;
}

void TOperationElementSharedState::UpdatePreemptableJobsList(
    double fairShareRatio,
    const TJobResources& totalResourceLimits,
    double preemptionSatisfactionThreshold,
    double aggressivePreemptionSatisfactionThreshold)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    auto getUsageRatio = [&] (const TJobResources& resourceUsage) {
        return GetDominantResourceUsage(resourceUsage, totalResourceLimits);
    };

    auto balanceLists = [&] (
        TJobIdList* left,
        TJobIdList* right,
        TJobResources resourceUsage,
        double fairShareRatioBound,
        std::function<void(TJobProperties*)> onMovedLeftToRight,
        std::function<void(TJobProperties*)> onMovedRightToLeft)
    {
        while (!left->empty()) {
            auto jobId = left->back();
            auto& jobProperties = JobPropertiesMap_.at(jobId);

            if (getUsageRatio(resourceUsage - jobProperties.ResourceUsage) < fairShareRatioBound) {
                break;
            }

            left->pop_back();
            right->push_front(jobId);
            jobProperties.JobIdListIterator = right->begin();
            onMovedLeftToRight(&jobProperties);

            resourceUsage -= jobProperties.ResourceUsage;
        }

        while (!right->empty()) {
            if (getUsageRatio(resourceUsage) >= fairShareRatioBound) {
                break;
            }

            auto jobId = right->front();
            auto& jobProperties = JobPropertiesMap_.at(jobId);

            right->pop_front();
            left->push_back(jobId);
            jobProperties.JobIdListIterator = --left->end();
            onMovedRightToLeft(&jobProperties);

            resourceUsage += jobProperties.ResourceUsage;
        }

        return resourceUsage;
    };

    // NB: We need 2 iteration since thresholds may change significantly such that we need
    // to move job from preemptable list to non-preemptable list through aggressively preemptable list.
    for (int iteration = 0; iteration < 2; ++iteration) {
        auto startNonPreemptableAndAggressivelyPreemptableResourceUsage_ = NonpreemptableResourceUsage_ + AggressivelyPreemptableResourceUsage_;

        NonpreemptableResourceUsage_ = balanceLists(
            &NonpreemptableJobs_,
            &AggressivelyPreemptableJobs_,
            NonpreemptableResourceUsage_,
            fairShareRatio * aggressivePreemptionSatisfactionThreshold,
            TJobProperties::SetAggressivelyPreemptable,
            TJobProperties::SetNonPreemptable);

        auto nonPreemptableAndAggressivelyPreemptableResourceUsage_ = balanceLists(
            &AggressivelyPreemptableJobs_,
            &PreemptableJobs_,
            startNonPreemptableAndAggressivelyPreemptableResourceUsage_,
            fairShareRatio * preemptionSatisfactionThreshold,
            TJobProperties::SetPreemptable,
            TJobProperties::SetAggressivelyPreemptable);

        AggressivelyPreemptableResourceUsage_ = nonPreemptableAndAggressivelyPreemptableResourceUsage_ - NonpreemptableResourceUsage_;
    }
}

bool TOperationElementSharedState::IsJobExisting(const TJobId& jobId) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return JobPropertiesMap_.find(jobId) != JobPropertiesMap_.end();
}

bool TOperationElementSharedState::IsJobPreemptable(const TJobId& jobId, bool aggressivePreemptionEnabled) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    if (aggressivePreemptionEnabled) {
        return JobPropertiesMap_.at(jobId).AggressivelyPreemptable;
    } else {
        return JobPropertiesMap_.at(jobId).Preemptable;
    }
}

int TOperationElementSharedState::GetRunningJobCount() const
{
    return RunningJobCount_;
}

int TOperationElementSharedState::GetPreemptableJobCount() const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return PreemptableJobs_.size();
}

int TOperationElementSharedState::GetAggressivelyPreemptableJobCount() const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return AggressivelyPreemptableJobs_.size();
}

TJobResources TOperationElementSharedState::AddJob(const TJobId& jobId, const TJobResources resourceUsage)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (Finalized_) {
        return ZeroJobResources();
    }

    PreemptableJobs_.push_back(jobId);

    auto it = JobPropertiesMap_.insert(std::make_pair(
        jobId,
        TJobProperties(
            /* preemptable */ true,
            /* aggressivelyPreemptable */ true,
            --PreemptableJobs_.end(),
            ZeroJobResources())));
    YCHECK(it.second);

    ++RunningJobCount_;

    IncreaseJobResourceUsage(it.first->second, resourceUsage);
    return resourceUsage;
}

TJobResources TOperationElement::Finalize()
{
    return SharedState_->Finalize();
}

TJobResources TOperationElementSharedState::RemoveJob(const TJobId& jobId)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (Finalized_) {
        return ZeroJobResources();
    }

    auto it = JobPropertiesMap_.find(jobId);
    YCHECK(it != JobPropertiesMap_.end());

    auto& properties = it->second;
    if (properties.Preemptable) {
        PreemptableJobs_.erase(properties.JobIdListIterator);
    } else if (properties.AggressivelyPreemptable) {
        AggressivelyPreemptableJobs_.erase(properties.JobIdListIterator);
    } else {
        NonpreemptableJobs_.erase(properties.JobIdListIterator);
    }

    --RunningJobCount_;

    auto resourceUsage = properties.ResourceUsage;
    IncreaseJobResourceUsage(properties, -resourceUsage);

    JobPropertiesMap_.erase(it);

    return resourceUsage;
}

bool TOperationElementSharedState::IsBlocked(
    NProfiling::TCpuInstant now,
    int maxConcurrentScheduleJobCalls,
    NProfiling::TCpuDuration scheduleJobFailBackoffTime) const
{
    return
        ConcurrentScheduleJobCalls_ >= maxConcurrentScheduleJobCalls ||
        LastScheduleJobFailTime_ + scheduleJobFailBackoffTime > now;
}


bool TOperationElementSharedState::TryStartScheduleJob(
    NProfiling::TCpuInstant now,
    int maxConcurrentScheduleJobCalls,
    NProfiling::TCpuDuration scheduleJobFailBackoffTime)
{
    if (IsBlocked(now, maxConcurrentScheduleJobCalls, scheduleJobFailBackoffTime)) {
        return false;
    }

    ++ConcurrentScheduleJobCalls_;
    return true;
}

void TOperationElementSharedState::FinishScheduleJob(
    bool enableBackoff,
    NProfiling::TCpuInstant now)
{
    --ConcurrentScheduleJobCalls_;

    if (enableBackoff) {
        LastScheduleJobFailTime_ = now;
    }
}

void TOperationElementSharedState::IncreaseJobResourceUsage(
    TJobProperties& properties,
    const TJobResources& resourcesDelta)
{
    properties.ResourceUsage += resourcesDelta;
    if (!properties.Preemptable) {
        NonpreemptableResourceUsage_ += resourcesDelta;
    }
}

////////////////////////////////////////////////////////////////////

TOperationElement::TOperationElement(
    TFairShareStrategyConfigPtr strategyConfig,
    TStrategyOperationSpecPtr spec,
    TOperationRuntimeParamsPtr runtimeParams,
    ISchedulerStrategyHost* host,
    TOperationPtr operation)
    : TSchedulerElement(host, strategyConfig)
    , TOperationElementFixedState(operation)
    , RuntimeParams_(runtimeParams)
    , Spec_(spec)
    , SchedulingTagFilter_(spec->SchedulingTagFilter)
    , SharedState_(New<TOperationElementSharedState>())
{ }

TOperationElement::TOperationElement(
    const TOperationElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElement(other, clonedParent)
    , TOperationElementFixedState(other)
    , RuntimeParams_(other.RuntimeParams_)
    , Spec_(other.Spec_)
    , SchedulingTagFilter_(other.SchedulingTagFilter_)
    , SharedState_(other.SharedState_)
{ }

double TOperationElement::GetFairShareStarvationTolerance() const
{
    return Spec_->FairShareStarvationTolerance.Get(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TOperationElement::GetMinSharePreemptionTimeout() const
{
    return Spec_->MinSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TOperationElement::GetFairSharePreemptionTimeout() const
{
    return Spec_->FairSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

void TOperationElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TSchedulerElement::UpdateBottomUp(dynamicAttributesList);

    Schedulable_ = Operation_->IsSchedulable();
    ResourceDemand_ = ComputeResourceDemand();
    ResourceLimits_ = ComputeResourceLimits();
    MaxPossibleResourceUsage_ = ComputeMaxPossibleResourceUsage();
    PendingJobCount_ = ComputePendingJobCount();
    StartTime_ = Operation_->GetStartTime();

    auto allocationLimits = GetAdjustedResourceLimits(
        ResourceDemand_,
        TotalResourceLimits_,
        GetHost()->GetExecNodeCount());

    auto dominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);
    auto dominantAllocationLimit = GetResource(allocationLimits, Attributes_.DominantResource);

    Attributes_.BestAllocationRatio =
        dominantLimit == 0 ? 1.0 : dominantAllocationLimit / dominantLimit;
}

void TOperationElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TSchedulerElement::UpdateTopDown(dynamicAttributesList);

    SharedState_->UpdatePreemptableJobsList(
        Attributes_.FairShareRatio,
        TotalResourceLimits_,
        StrategyConfig_->PreemptionSatisfactionThreshold,
        StrategyConfig_->AggressivePreemptionSatisfactionThreshold);
}

TJobResources TOperationElement::ComputePossibleResourceUsage(TJobResources limit) const
{
    auto usage = GetResourceUsage();
    limit = Min(limit, MaxPossibleResourceUsage() - usage);
    if (usage == ZeroJobResources()) {
        return limit;
    } else {
        return usage * GetMinResourceRatio(limit, usage);
    }
}

void TOperationElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    auto& attributes = dynamicAttributesList[GetTreeIndex()];
    attributes.Active = true;
    attributes.BestLeafDescendant = this;

    TSchedulerElement::UpdateDynamicAttributes(dynamicAttributesList);
}

void TOperationElement::PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled)
{
    auto& attributes = context.DynamicAttributes(this);

    attributes.Active = true;

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    if (StrategyConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context.CanSchedule[SchedulingTagFilterIndex_])
    {
        attributes.Active = false;
        return;
    }

    if (starvingOnly && !Starving_) {
        attributes.Active = false;
        return;
    }

    if (IsBlocked(context.SchedulingContext->GetNow())) {
        attributes.Active = false;
        return;
    }

    TSchedulerElement::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);
}

bool TOperationElement::ScheduleJob(TFairShareContext& context)
{
    YCHECK(IsActive(context.DynamicAttributesList));

    auto updateAncestorsAttributes = [&] () {
        auto* parent = GetParent();
        while (parent) {
            parent->UpdateDynamicAttributes(context.DynamicAttributesList);
            parent = parent->GetParent();
        }
    };

    auto disableOperationElement = [&] () {
        context.DynamicAttributes(this).Active = false;
        updateAncestorsAttributes();
    };

    auto now = context.SchedulingContext->GetNow();
    if (IsBlocked(now)) {
        disableOperationElement();
        return false;
    }

    if (!SharedState_->TryStartScheduleJob(
        now,
        StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
        NProfiling::DurationToCpuDuration(StrategyConfig_->ControllerScheduleJobFailBackoffTime)))
    {
        disableOperationElement();
        return false;
    }

    NProfiling::TScopedTimer timer;
    auto scheduleJobResult = DoScheduleJob(context);
    auto scheduleJobDuration = timer.GetElapsed();
    context.TotalScheduleJobDuration += scheduleJobDuration;
    context.ExecScheduleJobDuration += scheduleJobResult->Duration;

    for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
        context.FailedScheduleJob[reason] += scheduleJobResult->Failed[reason];
    }

    if (!scheduleJobResult->JobStartRequest) {
        disableOperationElement();

        bool enableBackoff = false;
        if (scheduleJobResult->Failed[EScheduleJobFailReason::NotEnoughResources] == 0 &&
            scheduleJobResult->Failed[EScheduleJobFailReason::NoLocalJobs] == 0)
        {
            LOG_DEBUG("Failed to schedule job, backing off (OperationId: %v, Reasons: %v)",
                OperationId_,
                scheduleJobResult->Failed);
            enableBackoff = true;
        }

        SharedState_->FinishScheduleJob(/*enableBackoff*/ enableBackoff, now);
        return false;
    }

    const auto& jobStartRequest = scheduleJobResult->JobStartRequest.Get();
    context.SchedulingContext->ResourceUsage() += jobStartRequest.ResourceLimits;
    OnJobStarted(jobStartRequest.Id, jobStartRequest.ResourceLimits);
    auto job = context.SchedulingContext->StartJob(OperationId_, jobStartRequest);

    UpdateDynamicAttributes(context.DynamicAttributesList);
    updateAncestorsAttributes();

    SharedState_->FinishScheduleJob(/*enableBackoff*/ false, now);
    return true;
}

Stroka TOperationElement::GetId() const
{
    return ToString(OperationId_);
}

double TOperationElement::GetWeight() const
{
    return RuntimeParams_->Weight;
}

double TOperationElement::GetMinShareRatio() const
{
    return Spec_->MinShareRatio;
}

TJobResources TOperationElement::GetMinShareResources() const
{
    return ToJobResources(Spec_->MinShareResources, ZeroJobResources());
}

double TOperationElement::GetMaxShareRatio() const
{
    return Spec_->MaxShareRatio;
}

const TSchedulingTagFilter& TOperationElement::GetSchedulingTagFilter() const
{
    return SchedulingTagFilter_;
}

ESchedulableStatus TOperationElement::GetStatus() const
{
    if (!Schedulable_) {
        return ESchedulableStatus::Normal;
    }

    if (GetPendingJobCount() == 0) {
        return ESchedulableStatus::Normal;
    }

    return TSchedulerElement::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

void TOperationElement::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    if (starving && !GetStarving()) {
        TSchedulerElement::SetStarving(true);
        LOG_INFO("Operation is now starving (OperationId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElement::SetStarving(false);
        LOG_INFO("Operation is no longer starving (OperationId: %v)",
            GetId());
    }
}

void TOperationElement::CheckForStarvation(TInstant now)
{
    YCHECK(!Cloned_);

    auto minSharePreemptionTimeout = Attributes_.AdjustedMinSharePreemptionTimeout;
    auto fairSharePreemptionTimeout = Attributes_.AdjustedFairSharePreemptionTimeout;

    double jobCountRatio = GetPendingJobCount() / StrategyConfig_->JobCountPreemptionTimeoutCoefficient;

    if (jobCountRatio < 1.0) {
        minSharePreemptionTimeout *= jobCountRatio;
        fairSharePreemptionTimeout *= jobCountRatio;
    }

    TSchedulerElement::CheckForStarvationImpl(
        minSharePreemptionTimeout,
        fairSharePreemptionTimeout,
        now);
}

bool TOperationElement::HasStarvingParent() const
{
    auto* parent = GetParent();
    while (parent) {
        if (parent->GetStarving()) {
            return true;
        }
        parent = parent->GetParent();
    }
    return false;
}

void TOperationElement::IncreaseResourceUsage(const TJobResources& delta)
{
    IncreaseLocalResourceUsage(delta);
    GetParent()->IncreaseResourceUsage(delta);
}

void TOperationElement::IncreaseJobResourceUsage(const TJobId& jobId, const TJobResources& resourcesDelta)
{
    auto delta = SharedState_->IncreaseJobResourceUsage(jobId, resourcesDelta);
    IncreaseResourceUsage(delta);
    SharedState_->UpdatePreemptableJobsList(
        Attributes_.FairShareRatio,
        TotalResourceLimits_,
        StrategyConfig_->PreemptionSatisfactionThreshold,
        StrategyConfig_->AggressivePreemptionSatisfactionThreshold);
}

bool TOperationElement::IsJobExisting(const TJobId& jobId) const
{
    return SharedState_->IsJobExisting(jobId);
}

bool TOperationElement::IsJobPreemptable(const TJobId& jobId, bool aggressivePreemptionEnabled) const
{
    return SharedState_->IsJobPreemptable(jobId, aggressivePreemptionEnabled);
}

int TOperationElement::GetRunningJobCount() const
{
    return SharedState_->GetRunningJobCount();
}

int TOperationElement::GetPreemptableJobCount() const
{
    return SharedState_->GetPreemptableJobCount();
}

int TOperationElement::GetAggressivelyPreemptableJobCount() const
{
    return SharedState_->GetAggressivelyPreemptableJobCount();
}

void TOperationElement::OnJobStarted(const TJobId& jobId, const TJobResources& resourceUsage)
{
    auto delta = SharedState_->AddJob(jobId, resourceUsage);
    IncreaseResourceUsage(delta);
}

void TOperationElement::OnJobFinished(const TJobId& jobId)
{
    auto resourceUsage = SharedState_->RemoveJob(jobId);
    IncreaseResourceUsage(-resourceUsage);
}

void TOperationElement::BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap)
{
    operationElementByIdMap->emplace(OperationId_, this);
}

TSchedulerElementPtr TOperationElement::Clone(TCompositeSchedulerElement* clonedParent)
{
    return New<TOperationElement>(*this, clonedParent);
}

bool TOperationElement::IsBlocked(NProfiling::TCpuInstant now) const
{
    return
        !Schedulable_ ||
        GetPendingJobCount() == 0 ||
        SharedState_->IsBlocked(
            now,
            StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
            NProfiling::DurationToCpuDuration(StrategyConfig_->ControllerScheduleJobFailBackoffTime));
}

TJobResources TOperationElement::GetHierarchicalResourceLimits(const TFairShareContext& context) const
{
    const auto& schedulingContext = context.SchedulingContext;

    // Bound limits with node free resources.
    auto limits =
        schedulingContext->ResourceLimits()
        - schedulingContext->ResourceUsage()
        + schedulingContext->ResourceUsageDiscount();

    // Bound limits with pool free resources.
    auto* parent = GetParent();
    while (parent) {
        auto parentLimits =
            parent->ResourceLimits()
            - parent->GetResourceUsage()
            + context.DynamicAttributes(parent).ResourceUsageDiscount;

        limits = Min(limits, parentLimits);
        parent = parent->GetParent();
    }

    // Bound limits with operation free resources.
    limits = Min(limits, ResourceLimits() - GetResourceUsage());

    return limits;
}

TScheduleJobResultPtr TOperationElement::DoScheduleJob(TFairShareContext& context)
{
    auto jobLimits = GetHierarchicalResourceLimits(context);

    auto scheduleJobResultFuture = BIND(&IOperationController::ScheduleJob, Controller_)
        .AsyncVia(Controller_->GetCancelableInvoker())
        .Run(context.SchedulingContext, jobLimits);

    auto scheduleJobResultFutureWithTimeout = scheduleJobResultFuture
        .WithTimeout(StrategyConfig_->ControllerScheduleJobTimeLimit);

    auto scheduleJobResultWithTimeoutOrError = WaitFor(scheduleJobResultFutureWithTimeout);

    if (!scheduleJobResultWithTimeoutOrError.IsOK()) {
        auto scheduleJobResult = New<TScheduleJobResult>();
        if (scheduleJobResultWithTimeoutOrError.GetCode() == NYT::EErrorCode::Timeout) {
            LOG_WARNING("Controller is scheduling for too long, aborting (OperationId: %v)",
                OperationId_);
            ++scheduleJobResult->Failed[EScheduleJobFailReason::Timeout];
            // If ScheduleJob was not canceled we need to abort created job.
            scheduleJobResultFuture.Subscribe(
                BIND([this_ = MakeStrong(this)] (const TErrorOr<TScheduleJobResultPtr>& scheduleJobResultOrError) {
                    if (scheduleJobResultOrError.IsOK()) {
                        const auto& scheduleJobResult = scheduleJobResultOrError.Value();
                        if (scheduleJobResult->JobStartRequest) {
                            const auto& jobId = scheduleJobResult->JobStartRequest->Id;
                            LOG_WARNING("Aborting late job (JobId: %v, OperationId: %v)",
                                jobId,
                                this_->OperationId_);
                            this_->Controller_->OnJobAborted(
                                std::make_unique<TAbortedJobSummary>(
                                    jobId,
                                    EAbortReason::SchedulingTimeout));
                        }
                    }
            }));
        }
        return scheduleJobResult;
    }

    auto scheduleJobResult = scheduleJobResultWithTimeoutOrError.Value();

    // Discard the job in case of resource overcommit.
    if (scheduleJobResult->JobStartRequest) {
        const auto& jobStartRequest = scheduleJobResult->JobStartRequest.Get();
        auto jobLimits = GetHierarchicalResourceLimits(context);
        if (!Dominates(jobLimits, jobStartRequest.ResourceLimits)) {
            const auto& jobId = scheduleJobResult->JobStartRequest->Id;
            LOG_DEBUG("Aborting job with resource overcommit: %v > %v (JobId: %v, OperationId: %v)",
                FormatResources(jobStartRequest.ResourceLimits),
                FormatResources(jobLimits),
                jobId,
                OperationId_);

            Controller_->GetCancelableInvoker()->Invoke(BIND(
                &IOperationController::OnJobAborted,
                Controller_,
                Passed(std::make_unique<TAbortedJobSummary>(
                    jobId,
                    EAbortReason::SchedulingResourceOvercommit))));

            // Reset result.
            scheduleJobResult = New<TScheduleJobResult>();
            ++scheduleJobResult->Failed[EScheduleJobFailReason::ResourceOvercommit];
        }
    }

    return scheduleJobResult;
}

TJobResources TOperationElement::ComputeResourceDemand() const
{
    if (Operation_->IsSchedulable()) {
        return GetResourceUsage() + Controller_->GetNeededResources();
    }
    return ZeroJobResources();
}

TJobResources TOperationElement::ComputeResourceLimits() const
{
    auto maxShareLimits = GetHost()->GetResourceLimits(GetSchedulingTagFilter()) * Spec_->MaxShareRatio;
    auto perTypeLimits = ToJobResources(Spec_->ResourceLimits, InfiniteJobResources());
    return Min(maxShareLimits, perTypeLimits);
}

TJobResources TOperationElement::ComputeMaxPossibleResourceUsage() const
{
    return Min(ResourceLimits(), ResourceDemand());
}

int TOperationElement::ComputePendingJobCount() const
{
    return Controller_->GetPendingJobCount();
}

////////////////////////////////////////////////////////////////////

TRootElement::TRootElement(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig)
    : TCompositeSchedulerElement(host, strategyConfig, RootPoolName)
{
    Attributes_.FairShareRatio = 1.0;
    Attributes_.GuaranteedResourcesRatio = 1.0;
    Attributes_.AdjustedMinShareRatio = 1.0;
    Attributes_.RecursiveMinShareRatio = 1.0;
    Mode_ = ESchedulingMode::FairShare;
    Attributes_.AdjustedFairShareStarvationTolerance = GetFairShareStarvationTolerance();
    Attributes_.AdjustedMinSharePreemptionTimeout = GetMinSharePreemptionTimeout();
    Attributes_.AdjustedFairSharePreemptionTimeout = GetFairSharePreemptionTimeout();
    AdjustedFairShareStarvationToleranceLimit_ = GetFairShareStarvationToleranceLimit();
    AdjustedMinSharePreemptionTimeoutLimit_ = GetMinSharePreemptionTimeoutLimit();
    AdjustedFairSharePreemptionTimeoutLimit_ = GetFairSharePreemptionTimeoutLimit();
}

TRootElement::TRootElement(const TRootElement& other)
    : TCompositeSchedulerElement(other, nullptr)
    , TRootElementFixedState(other)
{ }

void TRootElement::Update(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TreeSize_ = TCompositeSchedulerElement::EnumerateNodes(0);
    dynamicAttributesList.assign(TreeSize_, TDynamicAttributes());
    TCompositeSchedulerElement::Update(dynamicAttributesList);
}

bool TRootElement::IsRoot() const
{
    return true;
}

const TSchedulingTagFilter& TRootElement::GetSchedulingTagFilter() const
{
    return EmptySchedulingTagFilter;
}

Stroka TRootElement::GetId() const
{
    return Stroka(RootPoolName);
}

double TRootElement::GetWeight() const
{
    return 1.0;
}

double TRootElement::GetMinShareRatio() const
{
    return 1.0;
}

TJobResources TRootElement::GetMinShareResources() const
{
    return TotalResourceLimits_;
}

double TRootElement::GetMaxShareRatio() const
{
    return 1.0;
}

double TRootElement::GetFairShareStarvationTolerance() const
{
    return StrategyConfig_->FairShareStarvationTolerance;
}

TDuration TRootElement::GetMinSharePreemptionTimeout() const
{
    return StrategyConfig_->MinSharePreemptionTimeout;
}

TDuration TRootElement::GetFairSharePreemptionTimeout() const
{
    return StrategyConfig_->FairSharePreemptionTimeout;
}

void TRootElement::CheckForStarvation(TInstant now)
{
    Y_UNREACHABLE();
}

int TRootElement::GetMaxRunningOperationCount() const
{
    return StrategyConfig_->MaxRunningOperationCount;
}

int TRootElement::GetMaxOperationCount() const
{
    return StrategyConfig_->MaxOperationCount;
}

bool TRootElement::AreImmediateOperationsFobidden() const
{
    return StrategyConfig_->ForbidImmediateOperationsInRoot;
}

TSchedulerElementPtr TRootElement::Clone(TCompositeSchedulerElement* /*clonedParent*/)
{
    Y_UNREACHABLE();
}

TRootElementPtr TRootElement::Clone()
{
    return New<TRootElement>(*this);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
