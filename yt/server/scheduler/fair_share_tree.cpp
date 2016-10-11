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
    int treeSize)
    : SchedulingContext(schedulingContext)
    , DynamicAttributesList(treeSize)
{ }

TDynamicAttributes& TFairShareContext::DynamicAttributes(ISchedulerElement* element)
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

const TDynamicAttributes& TFairShareContext::DynamicAttributes(ISchedulerElement* element) const
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

////////////////////////////////////////////////////////////////////

TSchedulerElementBaseFixedState::TSchedulerElementBaseFixedState(ISchedulerStrategyHost* host)
    : Host_(host)
    , ResourceDemand_(ZeroJobResources())
    , ResourceLimits_(InfiniteJobResources())
    , MaxPossibleResourceUsage_(ZeroJobResources())
    , TotalResourceLimits_(host->GetTotalResourceLimits())
{ }

////////////////////////////////////////////////////////////////////

TSchedulerElementBaseSharedState::TSchedulerElementBaseSharedState()
    : ResourceUsage_(ZeroJobResources())
{ }

TJobResources TSchedulerElementBaseSharedState::GetResourceUsage()
{
    TReaderGuard guard(ResourceUsageLock_);

    return ResourceUsage_;
}

void TSchedulerElementBaseSharedState::IncreaseResourceUsage(const TJobResources& delta)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsage_ += delta;
}

double TSchedulerElementBaseSharedState::GetResourceUsageRatio(
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

const TNullable<Stroka> TSchedulerElementBase::NullNodeTag;

int TSchedulerElementBase::EnumerateNodes(int startIndex)
{
    YCHECK(!Cloned_);

    TreeIndex_ = startIndex++;
    return startIndex;
}

int TSchedulerElementBase::GetTreeIndex() const
{
    return TreeIndex_;
}

void TSchedulerElementBase::Update(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    UpdateBottomUp(dynamicAttributesList);
    UpdateTopDown(dynamicAttributesList);
}

void TSchedulerElementBase::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TotalResourceLimits_ = GetHost()->GetTotalResourceLimits();
    UpdateAttributes();
    dynamicAttributesList[this->GetTreeIndex()].Active = true;
    UpdateDynamicAttributes(dynamicAttributesList);
}

void TSchedulerElementBase::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);
}

void TSchedulerElementBase::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(IsActive(dynamicAttributesList));
    dynamicAttributesList[this->GetTreeIndex()].SatisfactionRatio = ComputeLocalSatisfactionRatio();
    dynamicAttributesList[this->GetTreeIndex()].Active = IsAlive();
}

void TSchedulerElementBase::PrescheduleJob(TFairShareContext& context, bool /*starvingOnly*/, bool /*aggressiveStarvationEnabled*/)
{
    UpdateDynamicAttributes(context.DynamicAttributesList);
}

const TSchedulableAttributes& TSchedulerElementBase::Attributes() const
{
    return Attributes_;
}

TSchedulableAttributes& TSchedulerElementBase::Attributes()
{
    return Attributes_;
}

void TSchedulerElementBase::UpdateAttributes()
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

    i64 dominantDemand = GetResource(demand, Attributes_.DominantResource);
    i64 dominantUsage = GetResource(usage, Attributes_.DominantResource);
    i64 dominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);

    Attributes_.DemandRatio =
        dominantLimit == 0 ? 1.0 : (double) dominantDemand / dominantLimit;

    double usageRatio =
        dominantLimit == 0 ? 0.0 : (double) dominantUsage / dominantLimit;

    Attributes_.DominantLimit = dominantLimit;

    Attributes_.MaxPossibleUsageRatio = GetMaxShareRatio();
    if (usageRatio > RatioComputationPrecision) {
        // In this case we know pool resource preferences and can take them into account.
        // We find maximum number K such that Usage * K < Limit and use it to estimate
        // maximum dominant resource usage.
        Attributes_.MaxPossibleUsageRatio = std::min(
            GetMinResourceRatio(maxPossibleResourceUsage, usage) * usageRatio,
            Attributes_.MaxPossibleUsageRatio);
    } else {
        // In this case we have no information about pool resource preferences, so just assume
        // that it uses all resources equally.
        Attributes_.MaxPossibleUsageRatio = std::min(
            Attributes_.DemandRatio,
            Attributes_.MaxPossibleUsageRatio);
    }
}

const TNullable<Stroka>& TSchedulerElementBase::GetNodeTag() const
{
    return NullNodeTag;
}

bool TSchedulerElementBase::IsActive(const TDynamicAttributesList& dynamicAttributesList) const
{
    return dynamicAttributesList[GetTreeIndex()].Active;
}

bool TSchedulerElementBase::IsAlive() const
{
    return SharedState_->GetAlive();
}

void TSchedulerElementBase::SetAlive(bool alive)
{
    SharedState_->SetAlive(alive);
}

TCompositeSchedulerElement* TSchedulerElementBase::GetParent() const
{
    return Parent_;
}

void TSchedulerElementBase::SetParent(TCompositeSchedulerElement* parent)
{
    YCHECK(!Cloned_);

    Parent_ = parent;
}

int TSchedulerElementBase::GetPendingJobCount() const
{
    return PendingJobCount_;
}

ESchedulableStatus TSchedulerElementBase::GetStatus() const
{
    return ESchedulableStatus::Normal;
}

bool TSchedulerElementBase::GetStarving() const
{
    return Starving_;
}

void TSchedulerElementBase::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    Starving_ = starving;
}

const TJobResources& TSchedulerElementBase::ResourceDemand() const
{
    return ResourceDemand_;
}

const TJobResources& TSchedulerElementBase::ResourceLimits() const
{
    return ResourceLimits_;
}

const TJobResources& TSchedulerElementBase::MaxPossibleResourceUsage() const
{
    return MaxPossibleResourceUsage_;
}

TJobResources TSchedulerElementBase::GetResourceUsage() const
{
    auto resourceUsage = SharedState_->GetResourceUsage();
    if (resourceUsage.GetUserSlots() > 0 && resourceUsage.GetMemory() == 0) {
        LOG_WARNING("Found usage of schedulable element %Qv with non-zero user slots and zero memory", GetId());
    }
    return resourceUsage;
}

double TSchedulerElementBase::GetResourceUsageRatio() const
{
    return SharedState_->GetResourceUsageRatio(
        Attributes_.DominantResource,
        Attributes_.DominantLimit);
}

void TSchedulerElementBase::IncreaseLocalResourceUsage(const TJobResources& delta)
{
    SharedState_->IncreaseResourceUsage(delta);
}

TSchedulerElementBase::TSchedulerElementBase(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig)
    : TSchedulerElementBaseFixedState(host)
    , StrategyConfig_(strategyConfig)
    , SharedState_(New<TSchedulerElementBaseSharedState>())
{ }

TSchedulerElementBase::TSchedulerElementBase(
    const TSchedulerElementBase& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElementBaseFixedState(other)
    , StrategyConfig_(other.StrategyConfig_)
    , SharedState_(other.SharedState_)
{
    Parent_ = clonedParent;
    Cloned_ = true;
}

ISchedulerStrategyHost* TSchedulerElementBase::GetHost() const
{
    YCHECK(!Cloned_);

    return Host_;
}

double TSchedulerElementBase::ComputeLocalSatisfactionRatio() const
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

ESchedulableStatus TSchedulerElementBase::GetStatus(double defaultTolerance) const
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

void TSchedulerElementBase::CheckForStarvationImpl(
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

TCompositeSchedulerElementFixedState::TCompositeSchedulerElementFixedState()
    : RunningOperationCount_(0)
    , OperationCount_(0)
{ }

////////////////////////////////////////////////////////////////////

TCompositeSchedulerElement::TCompositeSchedulerElement(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig,
    const Stroka& profilingName)
    : TSchedulerElementBase(host, strategyConfig)
    , ProfilingTag_(NProfiling::TProfileManager::Get()->RegisterTag("pool", profilingName))
{ }

TCompositeSchedulerElement::TCompositeSchedulerElement(
    const TCompositeSchedulerElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElementBase(other, clonedParent)
    , TCompositeSchedulerElementFixedState(other)
    , ProfilingTag_(other.ProfilingTag_)
{
    auto cloneChildren = [&] (
        const std::vector<ISchedulerElementPtr>& list,
        yhash_map<ISchedulerElementPtr, int>* clonedMap,
        std::vector<ISchedulerElementPtr>* clonedList)
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

    startIndex = TSchedulerElementBase::EnumerateNodes(startIndex);
    for (const auto& child : EnabledChildren_) {
        startIndex = child->EnumerateNodes(startIndex);
    }
    return startIndex;
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
    TSchedulerElementBase::UpdateBottomUp(dynamicAttributesList);
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

void TCompositeSchedulerElement::UpdateChildPreemptionSettings(const ISchedulerElementPtr& child)
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
    auto& attributes = dynamicAttributesList[this->GetTreeIndex()];

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    // Compute local satisfaction ratio.
    attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio();
    // Start times bubble up from leaf nodes with operations.
    attributes.MinSubtreeStartTime = TInstant::Max();
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

        // We need to evaluate both MinSubtreeStartTime and SatisfactionRatio
        // because parent can use different scheduling mode.
        attributes.MinSubtreeStartTime = std::min(
            attributes.MinSubtreeStartTime,
            bestChildAttributes.MinSubtreeStartTime);

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

    if (!context.SchedulingContext->CanSchedule(GetNodeTag())) {
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

    TSchedulerElementBase::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);
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

void TCompositeSchedulerElement::AddChild(const ISchedulerElementPtr& child, bool enabled)
{
    YCHECK(!Cloned_);

    auto& map = enabled ? EnabledChildToIndex_ : DisabledChildToIndex_;
    auto& list = enabled ? EnabledChildren_ : DisabledChildren_;
    AddChild(&map, &list, child);
}

void TCompositeSchedulerElement::EnableChild(const ISchedulerElementPtr& child)
{
    YCHECK(!Cloned_);

    RemoveChild(&DisabledChildToIndex_, &DisabledChildren_, child);
    AddChild(&EnabledChildToIndex_, &EnabledChildren_, child);
}

void TCompositeSchedulerElement::RemoveChild(const ISchedulerElementPtr& child)
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

    // TODO(acid): This code shouldn't use active children.
    const auto& bestChild = GetBestActiveChildFifo(dynamicAttributesList);
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        if (child == bestChild) {
            childAttributes.AdjustedMinShareRatio = std::min(
                childAttributes.DemandRatio,
                Attributes_.AdjustedMinShareRatio);
            childAttributes.FairShareRatio = std::min(
                childAttributes.DemandRatio,
                Attributes_.FairShareRatio);
        } else {
            childAttributes.AdjustedMinShareRatio = 0.0;
            childAttributes.FairShareRatio = 0.0;
        }
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
            "Impossible to satisfy resources guarantees for children of %Qv, ",
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
        [&] (double fitFactor, const ISchedulerElementPtr& child) -> double {
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
        [&] (const ISchedulerElementPtr& child, double value) {
            auto& attributes = child->Attributes();
            attributes.FairShareRatio = value;
        },
        Attributes_.FairShareRatio);

    // Compute guaranteed shares.
    ComputeByFitting(
        [&] (double fitFactor, const ISchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.AdjustedMinShareRatio);
            return result;
        },
        [&] (const ISchedulerElementPtr& child, double value) {
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

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChild(const TDynamicAttributesList& dynamicAttributesList) const
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

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const
{
    auto isBetter = [this, &dynamicAttributesList] (const ISchedulerElementPtr& lhs, const ISchedulerElementPtr& rhs) -> bool {
        for (auto parameter : FifoSortParameters_) {
            switch (parameter) {
                case EFifoSortParameter::Weight:
                    if (lhs->GetWeight() != rhs->GetWeight()) {
                        return lhs->GetWeight() > rhs->GetWeight();
                    }
                    break;
                case EFifoSortParameter::StartTime: {
                    const auto& lhsStartTime = dynamicAttributesList[lhs->GetTreeIndex()].MinSubtreeStartTime;
                    const auto& rhsStartTime = dynamicAttributesList[rhs->GetTreeIndex()].MinSubtreeStartTime;
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
    };

    ISchedulerElement* bestChild = nullptr;
    for (const auto& child : EnabledChildren_) {
        if (child->IsActive(dynamicAttributesList)) {
            if (bestChild && isBetter(bestChild, child))
                continue;

            bestChild = child.Get();
        }
    }
    return bestChild;
}

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const
{
    ISchedulerElement* bestChild = nullptr;
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
    const ISchedulerElementPtr& child)
{
    list->push_back(child);
    YCHECK(map->emplace(child, list->size() - 1).second);
}

void TCompositeSchedulerElement::RemoveChild(
    TChildMap* map,
    TChildList* list,
    const ISchedulerElementPtr& child)
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
    const ISchedulerElementPtr& child)
{
    return map.find(child) != map.end();
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
}

void TPool::SetDefaultConfig()
{
    YCHECK(!Cloned_);

    DoSetConfig(New<TPoolConfig>());
    DefaultConfigured_ = true;
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
    return TSchedulerElementBase::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
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
        TSchedulerElementBase::SetStarving(true);
        LOG_INFO("Pool is now starving (PoolId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElementBase::SetStarving(false);
        LOG_INFO("Pool is no longer starving (PoolId: %v)",
            GetId());
    }
}

void TPool::CheckForStarvation(TInstant now)
{
    YCHECK(!Cloned_);

    TSchedulerElementBase::CheckForStarvationImpl(
        Attributes_.AdjustedMinSharePreemptionTimeout,
        Attributes_.AdjustedFairSharePreemptionTimeout,
        now);
}

const TNullable<Stroka>& TPool::GetNodeTag() const
{
    return Config_->SchedulingTag;
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

ISchedulerElementPtr TPool::Clone(TCompositeSchedulerElement* clonedParent)
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
    auto resourceLimits = GetHost()->GetResourceLimits(GetNodeTag()) * Config_->MaxShareRatio;
    auto perTypeLimits = ToJobResources(Config_->ResourceLimits, InfiniteJobResources());
    return Min(resourceLimits, perTypeLimits);
}

////////////////////////////////////////////////////////////////////

TOperationElementFixedState::TOperationElementFixedState(TOperationPtr operation)
    : OperationId_(operation->GetId())
    , StartTime_(operation->GetStartTime())
    , IsSchedulable_(operation->IsSchedulable())
    , Operation_(operation.Get())
    , Controller_(operation->GetController())
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

    auto getUsageRatio = [&] (const TJobResources& resourcesUsage) {
        auto dominantResource = GetDominantResource(resourcesUsage, totalResourceLimits);
        i64 dominantLimit = GetResource(totalResourceLimits, dominantResource);
        i64 usage = GetResource(resourcesUsage, dominantResource);
        return dominantLimit == 0 ? 0.0 : (double) usage / dominantLimit;
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

    auto resourceUsage = properties.ResourceUsage;
    IncreaseJobResourceUsage(properties, -resourceUsage);

    JobPropertiesMap_.erase(it);

    return resourceUsage;
}

bool TOperationElementSharedState::IsBlocked(
    TInstant now,
    int maxConcurrentScheduleJobCalls,
    TDuration scheduleJobFailBackoffTime) const
{
    TReaderGuard guard(ConcurrentScheduleJobCallsLock_);

    return IsBlockedImpl(now, maxConcurrentScheduleJobCalls, scheduleJobFailBackoffTime);
}

bool TOperationElementSharedState::TryStartScheduleJob(
    TInstant now,
    int maxConcurrentScheduleJobCalls,
    TDuration scheduleJobFailBackoffTime)
{
    TWriterGuard guard(ConcurrentScheduleJobCallsLock_);

    if (IsBlockedImpl(now, maxConcurrentScheduleJobCalls, scheduleJobFailBackoffTime)) {
        return false;
    }

    BackingOff_ = false;
    ++ConcurrentScheduleJobCalls_;
    return true;
}

void TOperationElementSharedState::FinishScheduleJob(
    bool success,
    bool enableBackoff,
    TDuration scheduleJobDuration,
    TInstant now)
{
    TWriterGuard guard(ConcurrentScheduleJobCallsLock_);

    --ConcurrentScheduleJobCalls_;

    static const Stroka failPath = "/schedule_job/fail";
    static const Stroka successPath = "/schedule_job/success";
    const Stroka& path = success ? successPath : failPath;
    ControllerTimeStatistics_.AddSample(path, scheduleJobDuration.MicroSeconds());

    if (enableBackoff) {
        BackingOff_ = true;
        LastScheduleJobFailTime_ = now;
    }
}

TStatistics TOperationElementSharedState::GetControllerTimeStatistics()
{
    TReaderGuard guard(ConcurrentScheduleJobCallsLock_);

    return ControllerTimeStatistics_;
}

bool TOperationElementSharedState::IsBlockedImpl(
    TInstant now,
    int maxConcurrentScheduleJobCalls,
    TDuration scheduleJobFailBackoffTime) const
{
    return ConcurrentScheduleJobCalls_ >= maxConcurrentScheduleJobCalls ||
        (BackingOff_ && LastScheduleJobFailTime_ + scheduleJobFailBackoffTime > now);
}

void TOperationElementSharedState::IncreaseJobResourceUsage(TJobProperties& properties, const TJobResources& resourcesDelta)
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
    : TSchedulerElementBase(host, strategyConfig)
    , TOperationElementFixedState(operation)
    , RuntimeParams_(runtimeParams)
    , Spec_(spec)
    , SharedState_(New<TOperationElementSharedState>())
{ }

TOperationElement::TOperationElement(
    const TOperationElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElementBase(other, clonedParent)
    , TOperationElementFixedState(other)
    , RuntimeParams_(other.RuntimeParams_)
    , Spec_(other.Spec_)
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

    TSchedulerElementBase::UpdateBottomUp(dynamicAttributesList);

    IsSchedulable_ = Operation_->IsSchedulable();
    ResourceDemand_ = ComputeResourceDemand();
    ResourceLimits_ = ComputeResourceLimits();
    MaxPossibleResourceUsage_ = ComputeMaxPossibleResourceUsage();
    PendingJobCount_ = ComputePendingJobCount();

    auto allocationLimits = GetAdjustedResourceLimits(
        ResourceDemand_,
        TotalResourceLimits_,
        GetHost()->GetExecNodeCount());

    i64 dominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);
    i64 dominantAllocationLimit = GetResource(allocationLimits, Attributes_.DominantResource);

    Attributes_.BestAllocationRatio =
        dominantLimit == 0 ? 1.0 : (double) dominantAllocationLimit / dominantLimit;
}

void TOperationElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TSchedulerElementBase::UpdateTopDown(dynamicAttributesList);

    SharedState_->UpdatePreemptableJobsList(
        Attributes_.FairShareRatio,
        TotalResourceLimits_,
        StrategyConfig_->PreemptionSatisfactionThreshold,
        StrategyConfig_->AggressivePreemptionSatisfactionThreshold);
}

void TOperationElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    auto& attributes = dynamicAttributesList[this->GetTreeIndex()];
    attributes.Active = true;
    attributes.BestLeafDescendant = this;
    attributes.MinSubtreeStartTime = StartTime_;

    TSchedulerElementBase::UpdateDynamicAttributes(dynamicAttributesList);
}

void TOperationElement::PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled)
{
    auto& attributes = context.DynamicAttributes(this);

    attributes.Active = true;

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    if (!context.SchedulingContext->CanSchedule(GetNodeTag())) {
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

    TSchedulerElementBase::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);
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
    if (IsBlocked(now))
    {
        disableOperationElement();
        return false;
    }

    if (!SharedState_->TryStartScheduleJob(
        now,
        StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
        StrategyConfig_->ControllerScheduleJobFailBackoffTime))
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

        SharedState_->FinishScheduleJob(
            /*success*/ false,
            /*enableBackoff*/ enableBackoff,
            scheduleJobDuration,
            now);
        return false;
    }

    const auto& jobStartRequest = scheduleJobResult->JobStartRequest.Get();
    context.SchedulingContext->ResourceUsage() += jobStartRequest.ResourceLimits;
    OnJobStarted(jobStartRequest.Id, jobStartRequest.ResourceLimits);
    auto job = context.SchedulingContext->StartJob(OperationId_, jobStartRequest);
    context.JobToOperationElement[job] = this;

    UpdateDynamicAttributes(context.DynamicAttributesList);
    updateAncestorsAttributes();

    SharedState_->FinishScheduleJob(
        /*success*/ true,
        /*enableBackoff*/ false,
        scheduleJobDuration,
        now);
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

const TNullable<Stroka>& TOperationElement::GetNodeTag() const
{
    return Spec_->SchedulingTag;
}

ESchedulableStatus TOperationElement::GetStatus() const
{
    if (!IsSchedulable_) {
        return ESchedulableStatus::Normal;
    }

    if (GetPendingJobCount() == 0) {
        return ESchedulableStatus::Normal;
    }

    return TSchedulerElementBase::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

void TOperationElement::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    if (starving && !GetStarving()) {
        TSchedulerElementBase::SetStarving(true);
        LOG_INFO("Operation is now starving (OperationId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElementBase::SetStarving(false);
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

    TSchedulerElementBase::CheckForStarvationImpl(
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

TStatistics TOperationElement::GetControllerTimeStatistics()
{
    return SharedState_->GetControllerTimeStatistics();
}

void TOperationElement::BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap)
{
    operationElementByIdMap->emplace(OperationId_, this);
}

ISchedulerElementPtr TOperationElement::Clone(TCompositeSchedulerElement* clonedParent)
{
    return New<TOperationElement>(*this, clonedParent);
}

TOperation* TOperationElement::GetOperation() const
{
    YCHECK(!Cloned_);

    return Operation_;
}

bool TOperationElement::IsBlocked(TInstant now) const
{
    return !IsSchedulable_ ||
        GetPendingJobCount() == 0 ||
        SharedState_->IsBlocked(
            now,
            StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
            StrategyConfig_->ControllerScheduleJobFailBackoffTime);
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
            LOG_WARNING("Controller is scheduling for too long, aborting ScheduleJob");
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
    auto maxShareLimits = GetHost()->GetResourceLimits(GetNodeTag()) * Spec_->MaxShareRatio;
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

const TNullable<Stroka>& TRootElement::GetNodeTag() const
{
    return NullNodeTag;
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

ISchedulerElementPtr TRootElement::Clone(TCompositeSchedulerElement* /*clonedParent*/)
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
