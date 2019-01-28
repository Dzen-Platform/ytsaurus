#include "fair_share_tree_element.h"

#include "scheduling_context.h"

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/profile_manager.h>

#include <yt/core/misc/finally.h>

#include <yt/core/profiling/timing.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NYTree;
using namespace NProfiling;
using namespace NControllerAgent;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;
static const auto& Profiler = SchedulerProfiler;

////////////////////////////////////////////////////////////////////////////////

static const double RatioComputationPrecision = std::numeric_limits<double>::epsilon();
static const double RatioComparisonPrecision = sqrt(RatioComputationPrecision);

////////////////////////////////////////////////////////////////////////////////

static const char* MissingCustomProfilingTag = "missing";

TTagId GetCustomProfilingTag(const TString& tagName)
{
    static THashMap<TString, TTagId> tagNameToTagIdMap;

    auto it = tagNameToTagIdMap.find(tagName);
    if (it == tagNameToTagIdMap.end()) {
        it = tagNameToTagIdMap.emplace(
            tagName,
            TProfileManager::Get()->RegisterTag("custom", tagName)
        ).first;
    }
    return it->second;
};

////////////////////////////////////////////////////////////////////////////////

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
    if (config->Gpu) {
        defaultValue.SetGpu(*config->Gpu);
    }
    return defaultValue;
}

////////////////////////////////////////////////////////////////////////////////

TFairShareContext::TFairShareContext(const ISchedulingContextPtr& schedulingContext)
    : SchedulingContext(schedulingContext)
{ }

void TFairShareContext::Initialize(int treeSize, const std::vector<TSchedulingTagFilter>& registeredSchedulingTagFilters)
{
    YCHECK(!Initialized);

    Initialized = true;

    DynamicAttributesList.resize(treeSize);
    CanSchedule.reserve(registeredSchedulingTagFilters.size());
    for (const auto& filter : registeredSchedulingTagFilters) {
        CanSchedule.push_back(SchedulingContext->CanSchedule(filter));
    }
}

TDynamicAttributes& TFairShareContext::DynamicAttributesFor(const TSchedulerElement* element)
{
    int index = element->GetTreeIndex();
    YCHECK(index != UnassignedTreeIndex && index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

const TDynamicAttributes& TFairShareContext::DynamicAttributesFor(const TSchedulerElement* element) const
{
    int index = element->GetTreeIndex();
    YCHECK(index != UnassignedTreeIndex && index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerElementFixedState::TSchedulerElementFixedState(
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    const TFairShareStrategyTreeConfigPtr& treeConfig,
    const TString& treeId)
    : ResourceDemand_(ZeroJobResources())
    , ResourceLimits_(InfiniteJobResources())
    , MaxPossibleResourceUsage_(ZeroJobResources())
    , Host_(host)
    , TreeHost_(treeHost)
    , TreeConfig_(treeConfig)
    , TotalResourceLimits_(host->GetResourceLimits(treeConfig->NodesFilter))
    , TreeId_(treeId)
{ }

////////////////////////////////////////////////////////////////////////////////

TJobResources TSchedulerElementSharedState::GetResourceUsage()
{
    TReaderGuard guard(ResourceUsageLock_);

    return ResourceUsage_;
}

TJobResources TSchedulerElementSharedState::GetTotalResourceUsageWithPrecommit()
{
    TReaderGuard guard(ResourceUsageLock_);

    return ResourceUsage_ + ResourceUsagePrecommit_;
}

TJobMetrics TSchedulerElementSharedState::GetJobMetrics()
{
    TReaderGuard guard(JobMetricsLock_);

    return JobMetrics_;
}

void TSchedulerElementSharedState::CommitResourceUsage(
    const TJobResources& resourceUsageDelta,
    const TJobResources& precommittedResources)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsage_ += resourceUsageDelta;
    ResourceUsagePrecommit_ -= precommittedResources;
}

void TSchedulerElementSharedState::IncreaseResourceUsage(const TJobResources& delta)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsage_ += delta;
}

void TSchedulerElementSharedState::IncreaseResourceUsagePrecommit(const TJobResources& delta)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsagePrecommit_ += delta;
}

bool TSchedulerElementSharedState::CheckDemand(
    const TJobResources& delta,
    const TJobResources& resourceDemand,
    const TJobResources& resourceDiscount)
{
    TReaderGuard guard(ResourceUsageLock_);

    auto availableDemand = ComputeAvailableResources(
        resourceDemand,
        ResourceUsage_ + ResourceUsagePrecommit_,
        resourceDiscount);

    return Dominates(availableDemand, delta);
}

bool TSchedulerElementSharedState::TryIncreaseResourceUsagePrecommit(
    const TJobResources& delta,
    const TJobResources& resourceLimits,
    const TJobResources& resourceDiscount,
    TJobResources* availableResourceLimitsOutput)
{
    TWriterGuard guard(ResourceUsageLock_);

    auto availableResourceLimits = ComputeAvailableResources(
        resourceLimits,
        ResourceUsage_ + ResourceUsagePrecommit_,
        resourceDiscount);

    if (!Dominates(availableResourceLimits, delta)) {
        return false;
    }

    ResourceUsagePrecommit_ += delta;

    *availableResourceLimitsOutput = availableResourceLimits;
    return true;
}

void TSchedulerElementSharedState::ApplyJobMetricsDelta(const TJobMetrics& delta)
{
    TWriterGuard guard(JobMetricsLock_);

    JobMetrics_ += delta;
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

////////////////////////////////////////////////////////////////////////////////

int TSchedulerElement::EnumerateElements(int startIndex)
{
    YCHECK(!Cloned_);

    TreeIndex_ = startIndex++;
    return startIndex;
}

void TSchedulerElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    YCHECK(!Cloned_);

    TreeConfig_ = config;
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

    TotalResourceLimits_ = GetHost()->GetResourceLimits(TreeConfig_->NodesFilter);
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
    auto& attributes = dynamicAttributesList[GetTreeIndex()];
    YCHECK(attributes.Active);
    attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio();
    attributes.Active = IsAlive();
}

void TSchedulerElement::PrescheduleJob(TFairShareContext* context, bool /*starvingOnly*/, bool /*aggressiveStarvationEnabled*/)
{
    UpdateDynamicAttributes(context->DynamicAttributesList);
}

void TSchedulerElement::UpdateAttributes()
{
    YCHECK(!Cloned_);

    // Choose dominant resource types, compute max share ratios, compute demand ratios.
    const auto& demand = ResourceDemand();
    auto usage = GetLocalResourceUsage();

    auto maxPossibleResourceUsage = Min(TotalResourceLimits_, MaxPossibleResourceUsage_);

    if (usage == ZeroJobResources()) {
        Attributes_.DominantResource = GetDominantResource(demand, TotalResourceLimits_);
    } else {
        Attributes_.DominantResource = GetDominantResource(usage, TotalResourceLimits_);
    }

    Attributes_.DominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);

    auto dominantDemand = GetResource(demand, Attributes_.DominantResource);
    Attributes_.DemandRatio =
        Attributes_.DominantLimit == 0 ? 1.0 : dominantDemand / Attributes_.DominantLimit;

    auto possibleUsage = ComputePossibleResourceUsage(maxPossibleResourceUsage);
    double possibleUsageRatio = GetDominantResourceUsage(possibleUsage, TotalResourceLimits_);

    Attributes_.MaxPossibleUsageRatio = std::min(
        possibleUsageRatio,
        GetMaxShareRatio());
}

const TSchedulingTagFilter& TSchedulerElement::GetSchedulingTagFilter() const
{
    return EmptySchedulingTagFilter;
}

bool TSchedulerElement::IsRoot() const
{
    return false;
}

bool TSchedulerElement::IsOperation() const
{
    return false;
}


TString TSchedulerElement::GetLoggingAttributesString(const TDynamicAttributesList& dynamicAttributesList) const
{
    TDynamicAttributes dynamicAttributes;
    auto treeIndex = GetTreeIndex();
    if (treeIndex != UnassignedTreeIndex) {
        dynamicAttributes = dynamicAttributesList[treeIndex];
    }

    return Format(
        "Status: %v, DominantResource: %v, Demand: %.6lf, "
        "Usage: %.6lf, FairShare: %.6lf, Satisfaction: %.4lg, AdjustedMinShare: %.6lf, "
        "GuaranteedResourcesRatio: %.6lf, MaxPossibleUsage: %.6lf,  BestAllocation: %.6lf, "
        "Starving: %v, Weight: %v",
        GetStatus(),
        Attributes_.DominantResource,
        Attributes_.DemandRatio,
        GetLocalResourceUsageRatio(),
        Attributes_.FairShareRatio,
        dynamicAttributes.SatisfactionRatio,
        Attributes_.AdjustedMinShareRatio,
        Attributes_.GuaranteedResourcesRatio,
        Attributes_.MaxPossibleUsageRatio,
        Attributes_.BestAllocationRatio,
        GetStarving(),
        GetWeight());
}

TString TSchedulerElement::GetLoggingString(const TDynamicAttributesList& dynamicAttributesList) const
{
    return Format("Scheduling info for tree %Qv = {%v}", GetTreeId(), GetLoggingAttributesString(dynamicAttributesList));
}

bool TSchedulerElement::IsActive(const TDynamicAttributesList& dynamicAttributesList) const
{
    return dynamicAttributesList[GetTreeIndex()].Active;
}

double TSchedulerElement::GetWeight() const
{
    auto specifiedWeight = GetSpecifiedWeight();
    if (specifiedWeight) {
        return *specifiedWeight;
    }

    if (!TreeConfig_->InferWeightFromMinShareRatioMultiplier) {
        return 1.0;
    }
    if (Attributes().RecursiveMinShareRatio < RatioComputationPrecision) {
        return 1.0;
    }

    double parentMinShareRatio = 1.0;
    if (GetParent()) {
        parentMinShareRatio = GetParent()->Attributes().RecursiveMinShareRatio;
    }

    if (parentMinShareRatio < RatioComputationPrecision) {
        return 1.0;
    }

    return Attributes().RecursiveMinShareRatio * (*TreeConfig_->InferWeightFromMinShareRatioMultiplier) /
        parentMinShareRatio;
}

TCompositeSchedulerElement* TSchedulerElement::GetMutableParent()
{
    return Parent_;
}

const TCompositeSchedulerElement* TSchedulerElement::GetParent() const
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

TJobResources TSchedulerElement::GetLocalResourceUsage() const
{
    auto resourceUsage = SharedState_->GetResourceUsage();
    if (resourceUsage.GetUserSlots() > 0 && resourceUsage.GetMemory() == 0) {
        YT_LOG_WARNING("Found usage of schedulable element %Qv with non-zero "
            "user slots and zero memory (TreeId: %v)",
            GetId(),
            GetTreeId());
    }
    return resourceUsage;
}

TJobResources TSchedulerElement::GetTotalLocalResourceUsageWithPrecommit() const
{
    return SharedState_->GetTotalResourceUsageWithPrecommit();
}

TJobMetrics TSchedulerElement::GetJobMetrics() const
{
    return SharedState_->GetJobMetrics();
}

double TSchedulerElement::GetLocalResourceUsageRatio() const
{
    return SharedState_->GetResourceUsageRatio(
        Attributes_.DominantResource,
        Attributes_.DominantLimit);
}

TString TSchedulerElement::GetTreeId() const
{
    return TreeId_;
}

void TSchedulerElement::CommitLocalResourceUsage(
    const TJobResources& resourceUsageDelta,
    const TJobResources& precommittedResources)
{
    SharedState_->CommitResourceUsage(resourceUsageDelta, precommittedResources);
}

void TSchedulerElement::IncreaseLocalResourceUsage(const TJobResources& delta)
{
    SharedState_->IncreaseResourceUsage(delta);
}

void TSchedulerElement::IncreaseLocalResourceUsagePrecommit(const TJobResources& delta)
{
    SharedState_->IncreaseResourceUsagePrecommit(delta);
}

bool TSchedulerElement::CheckDemand(const TJobResources& delta, const TFairShareContext& context)
{
    return SharedState_->CheckDemand(delta, ResourceDemand(), context.DynamicAttributesFor(this).ResourceUsageDiscount);
}

bool TSchedulerElement::TryIncreaseLocalResourceUsagePrecommit(
    const TJobResources& delta,
    const TFairShareContext& context,
    TJobResources* availableResourceLimitsOutput)
{
    return SharedState_->TryIncreaseResourceUsagePrecommit(
        delta,
        ResourceLimits(),
        context.DynamicAttributesFor(this).ResourceUsageDiscount,
        availableResourceLimitsOutput);
}

void TSchedulerElement::ApplyJobMetricsDeltaLocal(const TJobMetrics& delta)
{
    SharedState_->ApplyJobMetricsDelta(delta);
}

TJobResources TSchedulerElement::GetLocalAvailableResourceDemand(const TFairShareContext& context) const
{
    return ComputeAvailableResources(
        ResourceDemand(),
        GetTotalLocalResourceUsageWithPrecommit(),
        context.DynamicAttributesFor(this).ResourceUsageDiscount);
}

TJobResources TSchedulerElement::GetLocalAvailableResourceLimits(const TFairShareContext& context) const
{
    return ComputeAvailableResources(
        ResourceLimits(),
        GetTotalLocalResourceUsageWithPrecommit(),
        context.DynamicAttributesFor(this).ResourceUsageDiscount);
}

void TSchedulerElement::IncreaseHierarchicalResourceUsage(const TJobResources& delta)
{
    auto* currentElement = this;
    while (currentElement) {
        currentElement->IncreaseLocalResourceUsage(delta);
        currentElement = currentElement->GetMutableParent();
    }
}

TSchedulerElement::TSchedulerElement(
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    const TFairShareStrategyTreeConfigPtr& treeConfig,
    const TString& treeId)
    : TSchedulerElementFixedState(host, treeHost, treeConfig, treeId)
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

IFairShareTreeHost* TSchedulerElement::GetTreeHost() const
{
    return TreeHost_;
}

double TSchedulerElement::ComputeLocalSatisfactionRatio() const
{
    double minShareRatio = Attributes_.AdjustedMinShareRatio;
    double fairShareRatio = Attributes_.FairShareRatio;
    double usageRatio = GetLocalResourceUsageRatio();

    // Check for corner cases.
    if (fairShareRatio < RatioComputationPrecision) {
        return std::numeric_limits<double>::max();
    }

    // Starvation is disabled for operations in FIFO pool.
    if (Attributes_.FifoIndex >= 0) {
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
    double usageRatio = GetLocalResourceUsageRatio();
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
            } else if (*BelowFairShareSince_ < now - minSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::BelowFairShare:
            if (!BelowFairShareSince_) {
                BelowFairShareSince_ = now;
            } else if (*BelowFairShareSince_ < now - fairSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::Normal:
            BelowFairShareSince_ = std::nullopt;
            SetStarving(false);
            break;

        default:
            Y_UNREACHABLE();
    }
}

void TSchedulerElement::SetOperationAlert(
    TOperationId operationId,
    EOperationAlertType alertType,
    const TError& alert,
    std::optional<TDuration> timeout)
{
    Host_->SetOperationAlert(operationId, alertType, alert, timeout);
}


TJobResources TSchedulerElement::ComputeResourceLimitsBase(const TResourceLimitsConfigPtr& resourceLimitsConfig) const
{
    auto connectionTime = InstantToCpuInstant(Host_->GetConnectionTime());
    auto delay = DurationToCpuDuration(TreeConfig_->TotalResourceLimitsConsiderDelay);
    auto maxShareLimits = connectionTime + delay < GetCpuInstant()
        ? GetHost()->GetResourceLimits(GetSchedulingTagFilter()) * GetMaxShareRatio()
        : InfiniteJobResources();
    auto perTypeLimits = ToJobResources(resourceLimitsConfig, InfiniteJobResources());
    return Min(maxShareLimits, perTypeLimits);
}

////////////////////////////////////////////////////////////////////////////////

TCompositeSchedulerElement::TCompositeSchedulerElement(
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    NProfiling::TTagId profilingTag,
    const TString& treeId)
    : TSchedulerElement(host, treeHost, treeConfig, treeId)
    , ProfilingTag_(profilingTag)
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
        THashMap<TSchedulerElementPtr, int>* clonedMap,
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

int TCompositeSchedulerElement::EnumerateElements(int startIndex)
{
    YCHECK(!Cloned_);

    startIndex = TSchedulerElement::EnumerateElements(startIndex);
    for (const auto& child : EnabledChildren_) {
        startIndex = child->EnumerateElements(startIndex);
    }
    return startIndex;
}

void TCompositeSchedulerElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    YCHECK(!Cloned_);

    TSchedulerElement::UpdateTreeConfig(config);

    auto updateChildrenConfig = [&config] (TChildList& list) {
        for (const auto& child : list) {
            child->UpdateTreeConfig(config);
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

void TCompositeSchedulerElement::IncreaseOperationCount(int delta)
{
    OperationCount_ += delta;

    auto parent = GetMutableParent();
    while (parent) {
        parent->OperationCount() += delta;
        parent = parent->GetMutableParent();
    }
}

void TCompositeSchedulerElement::IncreaseRunningOperationCount(int delta)
{
    RunningOperationCount_ += delta;

    auto parent = GetMutableParent();
    while (parent) {
        parent->RunningOperationCount() += delta;
        parent = parent->GetMutableParent();
    }
}

void TCompositeSchedulerElement::PrescheduleJob(TFairShareContext* context, bool starvingOnly, bool aggressiveStarvationEnabled)
{
    auto& attributes = context->DynamicAttributesFor(this);

    if (!IsAlive()) {
        ++context->DeactivationReasons[EDeactivationReason::IsNotAlive];
        attributes.Active = false;
        return;
    }

    if (TreeConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context->CanSchedule[SchedulingTagFilterIndex_])
    {
        ++context->DeactivationReasons[EDeactivationReason::UnmatchedSchedulingTag];
        attributes.Active = false;
        return;
    }

    attributes.Active = true;

    aggressiveStarvationEnabled = aggressiveStarvationEnabled || IsAggressiveStarvationEnabled();
    if (Starving_ && aggressiveStarvationEnabled) {
        context->SchedulingStatistics.HasAggressivelyStarvingElements = true;
    }

    // If pool is starving, any child will do.
    bool starvingOnlyForChildren = Starving_ ? false : starvingOnly;
    for (const auto& child : EnabledChildren_) {
        child->PrescheduleJob(context, starvingOnlyForChildren, aggressiveStarvationEnabled);
    }

    TSchedulerElement::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);

    if (attributes.Active) {
        ++context->ActiveTreeSize;
    }
}

bool TCompositeSchedulerElement::HasAggressivelyStarvingElements(TFairShareContext* context, bool aggressiveStarvationEnabled) const
{
    // TODO(ignat): eliminate copy/paste
    aggressiveStarvationEnabled = aggressiveStarvationEnabled || IsAggressiveStarvationEnabled();
    if (Starving_ && aggressiveStarvationEnabled) {
        return true;
    }

    for (const auto& child : EnabledChildren_) {
        if (child->HasAggressivelyStarvingElements(context, aggressiveStarvationEnabled)) {
            return true;
        }
    }

    return false;
}

bool TCompositeSchedulerElement::ScheduleJob(TFairShareContext* context)
{
    auto& attributes = context->DynamicAttributesFor(this);
    if (!attributes.Active) {
        return false;
    }

    auto bestLeafDescendant = attributes.BestLeafDescendant;
    if (!bestLeafDescendant->IsAlive()) {
        UpdateDynamicAttributes(context->DynamicAttributesList);
        if (!attributes.Active) {
            return false;
        }
        bestLeafDescendant = attributes.BestLeafDescendant;
    }

    // NB: Ignore the child's result.
    bestLeafDescendant->ScheduleJob(context);
    return true;
}

bool TCompositeSchedulerElement::IsExplicit() const
{
    return false;
}

bool TCompositeSchedulerElement::IsAggressiveStarvationEnabled() const
{
    return false;
}

bool TCompositeSchedulerElement::IsAggressiveStarvationPreemptionAllowed() const
{
    return true;
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

void TCompositeSchedulerElement::DisableChild(const TSchedulerElementPtr& child)
{
    YCHECK(!Cloned_);

    if (EnabledChildToIndex_.find(child) == EnabledChildToIndex_.end()) {
        return;
    }

    RemoveChild(&EnabledChildToIndex_, &EnabledChildren_, child);
    AddChild(&DisabledChildToIndex_, &DisabledChildren_, child);
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

    double resultSum = getSum(fitFactor);
    double uncertaintyRatio = 1.0;
    if (resultSum > RatioComputationPrecision && std::abs(sum - resultSum) > RatioComputationPrecision) {
        uncertaintyRatio = sum / resultSum;
    }

    // Compute actual min shares from fit factor.
    for (const auto& child : EnabledChildren_) {
        double value = getter(fitFactor, child);
        setter(child, value, uncertaintyRatio);
    }
}

void TCompositeSchedulerElement::UpdateFifo(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    auto children = EnabledChildren_;
    std::sort(children.begin(), children.end(), BIND(&TCompositeSchedulerElement::HasHigherPriorityInFifoMode, MakeStrong(this)));

    double remainingFairShareRatio = Attributes_.FairShareRatio;

    int index = 0;
    for (const auto& child : children) {
        auto& childAttributes = child->Attributes();

        childAttributes.RecursiveMinShareRatio = 0.0;
        childAttributes.AdjustedMinShareRatio = 0.0;

        childAttributes.FifoIndex = index;
        ++index;

        double childFairShareRatio = remainingFairShareRatio;
        childFairShareRatio = std::min(childFairShareRatio, childAttributes.MaxPossibleUsageRatio);
        childFairShareRatio = std::min(childFairShareRatio, childAttributes.BestAllocationRatio);
        child->SetFairShareRatio(childFairShareRatio);
        remainingFairShareRatio -= childFairShareRatio;
    }
}

void TCompositeSchedulerElement::UpdateFairShare(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    UpdateFairShareAlerts_.clear();

    // Compute min shares sum and min weight.
    double minShareRatioSumForPools = 0.0;
    double minShareRatioSumForOperations = 0.0;
    double minWeight = 1.0;
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        auto minShareRatio = child->GetMinShareRatio();
        auto minShareRatioByResources = GetMaxResourceRatio(child->GetMinShareResources(), TotalResourceLimits_);

        childAttributes.RecursiveMinShareRatio = std::max(
            Attributes_.RecursiveMinShareRatio * minShareRatio,
            minShareRatioByResources);

        if (child->IsOperation()) {
            minShareRatioSumForOperations += childAttributes.RecursiveMinShareRatio;
        } else {
            minShareRatioSumForPools += childAttributes.RecursiveMinShareRatio;
        }

        if ((!child->IsOperation() && minShareRatio > 0) && Attributes_.RecursiveMinShareRatio == 0) {
            UpdateFairShareAlerts_.emplace_back(
                "Min share ratio setting for %Qv has no effect "
                "because min share ratio of parent pool %Qv is zero",
                child->GetId(),
                GetId());
        }
        if ((!child->IsOperation() && minShareRatioByResources > 0) && Attributes_.RecursiveMinShareRatio == 0) {
            UpdateFairShareAlerts_.emplace_back(
                "Min share ratio resources setting for %Qv has no effect "
                "because min share ratio of parent pool %Qv is zero",
                child->GetId(),
                GetId());
        }

        if (child->GetWeight() > RatioComputationPrecision) {
            minWeight = std::min(minWeight, child->GetWeight());
        }
    }

    // If min share sum is larger than one, adjust all children min shares to sum up to one.
    if (minShareRatioSumForPools > Attributes_.RecursiveMinShareRatio + RatioComparisonPrecision) {
        UpdateFairShareAlerts_.emplace_back(
            "Impossible to satisfy resources guarantees of pool %Qv, "
            "total min share ratio of children pools is too large: %v > %v",
            GetId(),
            minShareRatioSumForPools,
            Attributes_.RecursiveMinShareRatio);

        double fitFactor = Attributes_.RecursiveMinShareRatio / minShareRatioSumForPools;
        for (const auto& child : EnabledChildren_) {
            auto& childAttributes = child->Attributes();
            if (child->IsOperation()) {
                childAttributes.RecursiveMinShareRatio = 0.0;
            } else {
                childAttributes.RecursiveMinShareRatio *= fitFactor;
            }
        }
    } else if (minShareRatioSumForPools + minShareRatioSumForOperations > Attributes_.RecursiveMinShareRatio + RatioComparisonPrecision) {
        // Min share ratios of operations are fitted silently.
        double fitFactor = (Attributes_.RecursiveMinShareRatio - minShareRatioSumForPools + RatioComparisonPrecision) / minShareRatioSumForOperations;
        for (const auto& child : EnabledChildren_) {
            auto& childAttributes = child->Attributes();
            if (child->IsOperation()) {
                childAttributes.RecursiveMinShareRatio *= fitFactor;
            }
        }
    }

    // Compute fair shares.
    ComputeByFitting(
        [&] (double fitFactor, const TSchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.RecursiveMinShareRatio);
            // Never give more than can be used.
            result = std::min(result, childAttributes.MaxPossibleUsageRatio);
            // Never give more than we can allocate.
            result = std::min(result, childAttributes.BestAllocationRatio);
            return result;
        },
        [&] (const TSchedulerElementPtr& child, double value, double uncertaintyRatio) {
            if (IsRoot() && uncertaintyRatio > 1.0) {
                uncertaintyRatio = 1.0;
            }
            child->SetFairShareRatio(value * uncertaintyRatio);
        },
        Attributes_.FairShareRatio);


    // Compute guaranteed shares.
    ComputeByFitting(
        [&] (double fitFactor, const TSchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.RecursiveMinShareRatio);
            return result;
        },
        [&] (const TSchedulerElementPtr& child, double value, double uncertaintyRatio) {
            auto& attributes = child->Attributes();
            attributes.GuaranteedResourcesRatio = value * uncertaintyRatio;
        },
        Attributes_.GuaranteedResourcesRatio);

    // Compute adjusted min share ratios.
    for (const auto& child : EnabledChildren_) {
        auto& childAttributes = child->Attributes();
        double result = childAttributes.RecursiveMinShareRatio;
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

////////////////////////////////////////////////////////////////////////////////

TPoolFixedState::TPoolFixedState(const TString& id)
    : Id_(id)
{ }

////////////////////////////////////////////////////////////////////////////////

TPool::TPool(
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    const TString& id,
    TPoolConfigPtr config,
    bool defaultConfigured,
    TFairShareStrategyTreeConfigPtr treeConfig,
    NProfiling::TTagId profilingTag,
    const TString& treeId)
    : TCompositeSchedulerElement(host, treeHost, treeConfig, profilingTag, treeId)
    , TPoolFixedState(id)
{
    DoSetConfig(config);
    DefaultConfigured_ = defaultConfigured;
}

TPool::TPool(const TPool& other, TCompositeSchedulerElement* clonedParent)
    : TCompositeSchedulerElement(other, clonedParent)
    , TPoolFixedState(other)
    , Config_(other.Config_)
    , SchedulingTagFilter_(other.SchedulingTagFilter_)
{ }

bool TPool::IsDefaultConfigured() const
{
    return DefaultConfigured_;
}

void TPool::SetUserName(const std::optional<TString>& userName)
{
    UserName_ = userName;
}

const std::optional<TString>& TPool::GetUserName() const
{
    return UserName_;
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

bool TPool::IsAggressiveStarvationPreemptionAllowed() const
{
    return Config_->AllowAggressiveStarvationPreemption.value_or(true);
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

TString TPool::GetId() const
{
    return Id_;
}

std::optional<double> TPool::GetSpecifiedWeight() const
{
    return Config_->Weight;
}

double TPool::GetMinShareRatio() const
{
    return Config_->MinShareRatio.value_or(0.0);
}

TJobResources TPool::GetMinShareResources() const
{
    return ToJobResources(Config_->MinShareResources, ZeroJobResources());
}

double TPool::GetMaxShareRatio() const
{
    return Config_->MaxShareRatio.value_or(1.0);
}

ESchedulableStatus TPool::GetStatus() const
{
    return TSchedulerElement::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

double TPool::GetFairShareStarvationTolerance() const
{
    return Config_->FairShareStarvationTolerance.value_or(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TPool::GetMinSharePreemptionTimeout() const
{
    return Config_->MinSharePreemptionTimeout.value_or(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TPool::GetFairSharePreemptionTimeout() const
{
    return Config_->FairSharePreemptionTimeout.value_or(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

double TPool::GetFairShareStarvationToleranceLimit() const
{
    return Config_->FairShareStarvationToleranceLimit.value_or(TreeConfig_->FairShareStarvationToleranceLimit);
}

TDuration TPool::GetMinSharePreemptionTimeoutLimit() const
{
    return Config_->MinSharePreemptionTimeoutLimit.value_or(TreeConfig_->MinSharePreemptionTimeoutLimit);
}

TDuration TPool::GetFairSharePreemptionTimeoutLimit() const
{
    return Config_->FairSharePreemptionTimeoutLimit.value_or(TreeConfig_->FairSharePreemptionTimeoutLimit);
}

void TPool::SetStarving(bool starving)
{
    YCHECK(!Cloned_);

    if (starving && !GetStarving()) {
        TSchedulerElement::SetStarving(true);
        YT_LOG_INFO("Pool is now starving (TreeId: %v, PoolId: %v, Status: %v)",
            GetTreeId(),
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElement::SetStarving(false);
        YT_LOG_INFO("Pool is no longer starving (TreeId: %v, PoolId: %v)",
            GetTreeId(),
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
    return Config_->MaxRunningOperationCount.value_or(TreeConfig_->MaxRunningOperationCountPerPool);
}

int TPool::GetMaxOperationCount() const
{
    return Config_->MaxOperationCount.value_or(TreeConfig_->MaxOperationCountPerPool);
}

std::vector<EFifoSortParameter> TPool::GetFifoSortParameters() const
{
    return FifoSortParameters_;
}

bool TPool::AreImmediateOperationsForbidden() const
{
    return Config_->ForbidImmediateOperations;
}

THashSet<TString> TPool::GetAllowedProfilingTags() const
{
    return Config_->AllowedProfilingTags;
}

TSchedulerElementPtr TPool::Clone(TCompositeSchedulerElement* clonedParent)
{
    return New<TPool>(*this, clonedParent);
}

void TPool::DoSetConfig(TPoolConfigPtr newConfig)
{
    YCHECK(!Cloned_);

    Config_ = std::move(newConfig);
    FifoSortParameters_ = Config_->FifoSortParameters;
    Mode_ = Config_->Mode;
    SchedulingTagFilter_ = TSchedulingTagFilter(Config_->SchedulingTagFilter);
}

TJobResources TPool::ComputeResourceLimits() const
{
    return ComputeResourceLimitsBase(Config_->ResourceLimits);
}

////////////////////////////////////////////////////////////////////////////////

TOperationElementFixedState::TOperationElementFixedState(
    IOperationStrategyHost* operation,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig)
    : OperationId_(operation->GetId())
    , Schedulable_(operation->IsSchedulable())
    , Operation_(operation)
    , ControllerConfig_(std::move(controllerConfig))
{ }

////////////////////////////////////////////////////////////////////////////////

TOperationElementSharedState::TOperationElementSharedState(int updatePreemptableJobsListLoggingPeriod)
    : UpdatePreemptableJobsListLoggingPeriod_(updatePreemptableJobsListLoggingPeriod)
{ }

TJobResources TOperationElementSharedState::Disable()
{
    TWriterGuard guard(JobPropertiesMapLock_);

    Enabled_ = false;

    auto resourceUsage = ZeroJobResources();
    for (const auto& pair : JobPropertiesMap_) {
        resourceUsage += pair.second.ResourceUsage;
    }

    NonpreemptableResourceUsage_ = ZeroJobResources();
    AggressivelyPreemptableResourceUsage_ = ZeroJobResources();
    RunningJobCount_ = 0;
    PreemptableJobs_.clear();
    AggressivelyPreemptableJobs_.clear();
    NonpreemptableJobs_.clear();
    JobPropertiesMap_.clear();

    return resourceUsage;
}

void TOperationElementSharedState::Enable()
{
    TWriterGuard guard(JobPropertiesMapLock_);

    YCHECK(!Enabled_);
    Enabled_ = true;
}

TJobResources TOperationElementSharedState::IncreaseJobResourceUsage(
    TJobId jobId,
    const TJobResources& resourcesDelta)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return ZeroJobResources();
    }

    IncreaseJobResourceUsage(GetJobProperties(jobId), resourcesDelta);
    return resourcesDelta;
}

void TOperationElementSharedState::UpdatePreemptableJobsList(
    double fairShareRatio,
    const TJobResources& totalResourceLimits,
    double preemptionSatisfactionThreshold,
    double aggressivePreemptionSatisfactionThreshold,
    int* moveCount)
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
            auto* jobProperties = GetJobProperties(jobId);

            if (getUsageRatio(resourceUsage - jobProperties->ResourceUsage) < fairShareRatioBound) {
                break;
            }

            left->pop_back();
            right->push_front(jobId);
            jobProperties->JobIdListIterator = right->begin();
            onMovedLeftToRight(jobProperties);

            resourceUsage -= jobProperties->ResourceUsage;
            ++(*moveCount);
        }

        while (!right->empty()) {
            if (getUsageRatio(resourceUsage) >= fairShareRatioBound) {
                break;
            }

            auto jobId = right->front();
            auto* jobProperties = GetJobProperties(jobId);

            right->pop_front();
            left->push_back(jobId);
            jobProperties->JobIdListIterator = --left->end();
            onMovedRightToLeft(jobProperties);

            resourceUsage += jobProperties->ResourceUsage;
            ++(*moveCount);
        }

        return resourceUsage;
    };

    auto setPreemptable = [] (TJobProperties* properties) {
        properties->Preemptable = true;
        properties->AggressivelyPreemptable = true;
    };

    auto setAggressivelyPreemptable = [] (TJobProperties* properties) {
        properties->Preemptable = false;
        properties->AggressivelyPreemptable = true;
    };

    auto setNonPreemptable = [] (TJobProperties* properties) {
        properties->Preemptable = false;
        properties->AggressivelyPreemptable = false;
    };

    bool enableLogging = (UpdatePreemptableJobsListCount_.fetch_add(1) % UpdatePreemptableJobsListLoggingPeriod_) == 0;

    YT_LOG_DEBUG_IF(enableLogging,
        "Update preemptable job lists inputs (FairShareRatio: %v, TotalResourceLimits: %v, "
        "PreemptionSatisfactionThreshold: %v, AggressivePreemptionSatisfactionThreshold: %v)",
        fairShareRatio,
        FormatResources(totalResourceLimits),
        preemptionSatisfactionThreshold,
        aggressivePreemptionSatisfactionThreshold);

    // NB: We need 2 iterations since thresholds may change significantly such that we need
    // to move job from preemptable list to non-preemptable list through aggressively preemptable list.
    for (int iteration = 0; iteration < 2; ++iteration) {
        YT_LOG_DEBUG_IF(enableLogging,
            "Preemptable lists usage bounds before update (NonpreemptableResourceUsage: %v, AggressivelyPreemptableResourceUsage: %v, Iteration: %v)",
            FormatResources(NonpreemptableResourceUsage_),
            FormatResources(AggressivelyPreemptableResourceUsage_),
            iteration);

        auto startNonPreemptableAndAggressivelyPreemptableResourceUsage_ = NonpreemptableResourceUsage_ + AggressivelyPreemptableResourceUsage_;

        NonpreemptableResourceUsage_ = balanceLists(
            &NonpreemptableJobs_,
            &AggressivelyPreemptableJobs_,
            NonpreemptableResourceUsage_,
            fairShareRatio * aggressivePreemptionSatisfactionThreshold,
            setAggressivelyPreemptable,
            setNonPreemptable);

        auto nonpreemptableAndAggressivelyPreemptableResourceUsage_ = balanceLists(
            &AggressivelyPreemptableJobs_,
            &PreemptableJobs_,
            startNonPreemptableAndAggressivelyPreemptableResourceUsage_,
            fairShareRatio * preemptionSatisfactionThreshold,
            setPreemptable,
            setAggressivelyPreemptable);

        AggressivelyPreemptableResourceUsage_ = nonpreemptableAndAggressivelyPreemptableResourceUsage_ - NonpreemptableResourceUsage_;
    }

    YT_LOG_DEBUG_IF(enableLogging,
        "Preemptable lists usage bounds after update (NonpreemptableResourceUsage: %v, AggressivelyPreemptableResourceUsage: %v)",
        FormatResources(NonpreemptableResourceUsage_),
        FormatResources(AggressivelyPreemptableResourceUsage_));
}

bool TOperationElementSharedState::IsJobKnown(TJobId jobId) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return JobPropertiesMap_.find(jobId) != JobPropertiesMap_.end();
}

bool TOperationElementSharedState::IsJobPreemptable(TJobId jobId, bool aggressivePreemptionEnabled) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return false;
    }

    const auto* properties = GetJobProperties(jobId);
    return aggressivePreemptionEnabled ? properties->AggressivelyPreemptable : properties->Preemptable;
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

std::optional<TJobResources> TOperationElementSharedState::AddJob(TJobId jobId, const TJobResources& resourceUsage, bool force)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (!Enabled_ && !force) {
        return std::nullopt;
    }

    LastScheduleJobSuccessTime_ = TInstant::Now();

    PreemptableJobs_.push_back(jobId);

    auto it = JobPropertiesMap_.emplace(
        jobId,
        TJobProperties(
            /* preemptable */ true,
            /* aggressivelyPreemptable */ true,
            --PreemptableJobs_.end(),
            ZeroJobResources()));
    YCHECK(it.second);

    ++RunningJobCount_;

    IncreaseJobResourceUsage(&it.first->second, resourceUsage);
    return resourceUsage;
}

void TOperationElementSharedState::UpdatePreemptionStatusStatistics(EOperationPreemptionStatus status)
{
    auto guard = Guard(PreemptionStatusStatisticsLock_);

    PreemptionStatusStatistics_[status] += 1;
}

TPreemptionStatusStatisticsVector TOperationElementSharedState::GetPreemptionStatusStatistics() const
{
    auto guard = Guard(PreemptionStatusStatisticsLock_);

    return PreemptionStatusStatistics_;
}

void TOperationElementSharedState::OnOperationDeactivated(EDeactivationReason reason)
{
    ++DeactivationReasons_[reason];
    ++DeactivationReasonsFromLastNonStarvingTime_[reason];
}

TEnumIndexedVector<int, EDeactivationReason> TOperationElementSharedState::GetDeactivationReasons() const
{
    TEnumIndexedVector<int, EDeactivationReason> result;
    for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
        result[reason] = DeactivationReasons_[reason];
    }
    return result;
}

TEnumIndexedVector<int, EDeactivationReason> TOperationElementSharedState::GetDeactivationReasonsFromLastNonStarvingTime() const
{
    TEnumIndexedVector<int, EDeactivationReason> result;
    for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
        result[reason] = DeactivationReasonsFromLastNonStarvingTime_[reason];
    }
    return result;
}

void TOperationElementSharedState::ResetDeactivationReasonsFromLastNonStarvingTime()
{
    for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
        DeactivationReasonsFromLastNonStarvingTime_[reason] = 0;
    }
}

TInstant TOperationElementSharedState::GetLastScheduleJobSuccessTime() const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return LastScheduleJobSuccessTime_;
}

void TOperationElement::OnOperationDeactivated(EDeactivationReason reason)
{
    SharedState_->OnOperationDeactivated(reason);
}

TEnumIndexedVector<int, EDeactivationReason> TOperationElement::GetDeactivationReasons() const
{
    return SharedState_->GetDeactivationReasons();
}

TEnumIndexedVector<int, EDeactivationReason> TOperationElement::GetDeactivationReasonsFromLastNonStarvingTime() const
{
    return SharedState_->GetDeactivationReasonsFromLastNonStarvingTime();
}

std::optional<NProfiling::TTagId> TOperationElement::GetCustomProfilingTag()
{
    if (GetParent() == nullptr) {
        return std::nullopt;
    }

    auto tagName = Spec_->CustomProfilingTag;
    THashSet<TString> allowedProfilingTags;
    auto parent = GetParent();
    while (parent) {
        for (const auto& tag : parent->GetAllowedProfilingTags()) {
            allowedProfilingTags.insert(tag);
        }
        parent = parent->GetParent();
    }
    if (tagName && (
            allowedProfilingTags.find(*tagName) == allowedProfilingTags.end() ||
            (TreeConfig_->CustomProfilingTagFilter && NRe2::TRe2::FullMatch(NRe2::StringPiece(*tagName), *TreeConfig_->CustomProfilingTagFilter))
        ))
    {
        tagName = std::nullopt;
    }

    if (tagName) {
        return NScheduler::GetCustomProfilingTag(*tagName);
    } else {
        return NScheduler::GetCustomProfilingTag(MissingCustomProfilingTag);
    }
}

bool TOperationElement::IsOperation() const
{
    return true;
}

void TOperationElement::Disable()
{
    YT_LOG_DEBUG("Operation element disabled in strategy (OperationId: %v)", OperationId_);

    auto delta = SharedState_->Disable();
    IncreaseLocalResourceUsage(-delta);
}

void TOperationElement::Enable()
{
    YT_LOG_DEBUG("Operation element enabled in strategy (OperationId: %v)", OperationId_);

    return SharedState_->Enable();
}

std::optional<TJobResources> TOperationElementSharedState::RemoveJob(TJobId jobId)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return std::nullopt;
    }

    auto it = JobPropertiesMap_.find(jobId);
    YCHECK(it != JobPropertiesMap_.end());

    auto* properties = &it->second;
    if (properties->Preemptable) {
        PreemptableJobs_.erase(properties->JobIdListIterator);
    } else if (properties->AggressivelyPreemptable) {
        AggressivelyPreemptableJobs_.erase(properties->JobIdListIterator);
    } else {
        NonpreemptableJobs_.erase(properties->JobIdListIterator);
    }

    --RunningJobCount_;

    auto resourceUsage = properties->ResourceUsage;
    IncreaseJobResourceUsage(properties, -resourceUsage);

    JobPropertiesMap_.erase(it);

    return resourceUsage;
}

std::optional<EDeactivationReason> TOperationElement::TryStartScheduleJob(
    NProfiling::TCpuInstant now,
    const TJobResources& minNeededResources,
    const TFairShareContext& context,
    TJobResources* availableResourcesOutput)
{
    auto blocked = Controller_->IsBlocked(
        now,
        Spec_->MaxConcurrentControllerScheduleJobCalls.value_or(ControllerConfig_->MaxConcurrentControllerScheduleJobCalls),
        ControllerConfig_->ScheduleJobFailBackoffTime);
    if (blocked) {
        return EDeactivationReason::IsBlocked;
    }

    auto nodeFreeResources = context.SchedulingContext->GetNodeFreeResourcesWithDiscount();
    if (!Dominates(nodeFreeResources, minNeededResources)) {
        return EDeactivationReason::MinNeededResourcesUnsatisfied;
    }

    // Do preliminary checks to avoid the overhead of updating and reverting precommit usage.
    auto availableResources = GetHierarchicalAvailableResources(context);
    auto availableDemand = GetLocalAvailableResourceDemand(context);
    if (!Dominates(availableResources, minNeededResources) || !Dominates(availableDemand, minNeededResources)) {
        return EDeactivationReason::ResourceLimitsExceeded;
    }

    TJobResources availableResourceLimits;
    if (!TryIncreaseHierarchicalResourceUsagePrecommit(
            minNeededResources,
            context,
            /* checkDemand */ true,
            &availableResourceLimits)) {
        return EDeactivationReason::ResourceLimitsExceeded;
    }

    Controller_->IncreaseConcurrentScheduleJobCalls();

    *availableResourcesOutput = Min(availableResourceLimits, nodeFreeResources);
    return std::nullopt;
}

void TOperationElement::FinishScheduleJob(
    bool enableBackoff,
    NProfiling::TCpuInstant now)
{
    Controller_->DecreaseConcurrentScheduleJobCalls();

    if (enableBackoff) {
        Controller_->SetLastScheduleJobFailTime(now);
    }

    LastScheduleJobSuccessTime_ = CpuInstantToInstant(now);
}

void TOperationElementSharedState::IncreaseJobResourceUsage(
    TJobProperties* properties,
    const TJobResources& resourcesDelta)
{
    properties->ResourceUsage += resourcesDelta;
    if (!properties->Preemptable) {
        if (properties->AggressivelyPreemptable) {
            AggressivelyPreemptableResourceUsage_ += resourcesDelta;
        } else {
            NonpreemptableResourceUsage_ += resourcesDelta;
        }
    }
}

TOperationElementSharedState::TJobProperties* TOperationElementSharedState::GetJobProperties(TJobId jobId)
{
    auto it = JobPropertiesMap_.find(jobId);
    Y_ASSERT(it != JobPropertiesMap_.end());
    return &it->second;
}

const TOperationElementSharedState::TJobProperties* TOperationElementSharedState::GetJobProperties(TJobId jobId) const
{
    auto it = JobPropertiesMap_.find(jobId);
    Y_ASSERT(it != JobPropertiesMap_.end());
    return &it->second;
}

////////////////////////////////////////////////////////////////////////////////

TOperationElement::TOperationElement(
    TFairShareStrategyTreeConfigPtr treeConfig,
    TStrategyOperationSpecPtr spec,
    TOperationFairShareTreeRuntimeParametersPtr runtimeParams,
    TFairShareStrategyOperationControllerPtr controller,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig,
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    IOperationStrategyHost* operation,
    const TString& treeId)
    : TSchedulerElement(host, treeHost, treeConfig, treeId)
    , TOperationElementFixedState(operation, controllerConfig)
    , RuntimeParams_(std::move(runtimeParams))
    , Spec_(spec)
    , SharedState_(New<TOperationElementSharedState>(spec->UpdatePreemptableJobsListLoggingPeriod))
    , Controller_(std::move(controller))
    , SchedulingTagFilter_(spec->SchedulingTagFilter)
    , LastNonStarvingTime_(TInstant::Now())
{ }

TOperationElement::TOperationElement(
    const TOperationElement& other,
    TCompositeSchedulerElement* clonedParent)
    : TSchedulerElement(other, clonedParent)
    , TOperationElementFixedState(other)
    , RuntimeParams_(other.RuntimeParams_)
    , Spec_(other.Spec_)
    , SharedState_(other.SharedState_)
    , Controller_(other.Controller_)
    , SchedulingTagFilter_(other.SchedulingTagFilter_)
    , LastNonStarvingTime_(other.LastNonStarvingTime_)
{ }

double TOperationElement::GetFairShareStarvationTolerance() const
{
    return Spec_->FairShareStarvationTolerance.value_or(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TOperationElement::GetMinSharePreemptionTimeout() const
{
    return Spec_->MinSharePreemptionTimeout.value_or(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TOperationElement::GetFairSharePreemptionTimeout() const
{
    return Spec_->FairSharePreemptionTimeout.value_or(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

void TOperationElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    Schedulable_ = Operation_->IsSchedulable();
    ResourceDemand_ = ComputeResourceDemand();
    ResourceLimits_ = ComputeResourceLimits();
    MaxPossibleResourceUsage_ = ComputeMaxPossibleResourceUsage();
    PendingJobCount_ = ComputePendingJobCount();
    StartTime_ = Operation_->GetStartTime();

    // It should be called after update of ResourceDemand_ and MaxPossibleResourceUsage_ since
    // these fields are used to calculate dominant resource.
    TSchedulerElement::UpdateBottomUp(dynamicAttributesList);

    auto allocationLimits = GetAdjustedResourceLimits(
        ResourceDemand_,
        TotalResourceLimits_,
        GetHost()->GetExecNodeMemoryDistribution(SchedulingTagFilter_ & TreeConfig_->NodesFilter));

    auto dominantLimit = GetResource(TotalResourceLimits_, Attributes_.DominantResource);
    auto dominantAllocationLimit = GetResource(allocationLimits, Attributes_.DominantResource);

    Attributes_.BestAllocationRatio =
        dominantLimit == 0 ? 1.0 : dominantAllocationLimit / dominantLimit;
}

void TOperationElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TSchedulerElement::UpdateTopDown(dynamicAttributesList);

    UpdatePreemptableJobsList();
}

TJobResources TOperationElement::ComputePossibleResourceUsage(TJobResources limit) const
{
    auto usage = GetLocalResourceUsage();
    if (!Dominates(limit, usage)) {
        return usage * GetMinResourceRatio(limit, usage);
    } else {
        auto remainingDemand = ResourceDemand() - usage;
        if (remainingDemand == ZeroJobResources()) {
            return usage;
        }

        auto remainingLimit = Max(ZeroJobResources(), limit - usage);
        // TODO(asaitgalin): Move this to MaxPossibleResourceUsage computation.
        return Min(ResourceDemand(), usage + remainingDemand * GetMinResourceRatio(remainingLimit, remainingDemand));
    }
}

bool TOperationElement::HasJobsSatisfyingResourceLimits(const TFairShareContext& context) const
{
    for (const auto& jobResources : Controller_->GetDetailedMinNeededJobResources()) {
        if (context.SchedulingContext->CanStartJob(jobResources)) {
            return true;
        }
    }
    return false;
}

void TOperationElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    auto& attributes = dynamicAttributesList[GetTreeIndex()];
    attributes.Active = true;
    attributes.BestLeafDescendant = this;

    TSchedulerElement::UpdateDynamicAttributes(dynamicAttributesList);
}

void TOperationElement::UpdateControllerConfig(const TFairShareStrategyOperationControllerConfigPtr& config)
{
    YCHECK(!Cloned_);
    ControllerConfig_ = config;
}

void TOperationElement::PrescheduleJob(TFairShareContext* context, bool starvingOnly, bool aggressiveStarvationEnabled)
{
    auto& attributes = context->DynamicAttributesFor(this);

    attributes.Active = true;

    auto onOperationDeactivated = [&] (EDeactivationReason reason) {
        ++context->DeactivationReasons[reason];
        OnOperationDeactivated(reason);
        attributes.Active = false;
    };

    if (!IsAlive()) {
        onOperationDeactivated(EDeactivationReason::IsNotAlive);
        return;
    }

    if (TreeConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context->CanSchedule[SchedulingTagFilterIndex_])
    {
        onOperationDeactivated(EDeactivationReason::UnmatchedSchedulingTag);
        return;
    }

    if (starvingOnly && !Starving_) {
        onOperationDeactivated(EDeactivationReason::IsNotStarving);
        return;
    }

    if (IsBlocked(context->SchedulingContext->GetNow())) {
        onOperationDeactivated(EDeactivationReason::IsBlocked);
        return;
    }

	if (Controller_->IsSaturatedInTentativeTree(
        context->SchedulingContext->GetNow(),
        TreeId_,
        TreeConfig_->TentativeTreeSaturationDeactivationPeriod))
    {
        onOperationDeactivated(EDeactivationReason::SaturatedInTentativeTree);
        return;
    }

    ++context->ActiveTreeSize;
    ++context->ActiveOperationCount;

    TSchedulerElement::PrescheduleJob(context, starvingOnly, aggressiveStarvationEnabled);
}

bool TOperationElement::HasAggressivelyStarvingElements(TFairShareContext* /*context*/, bool /*aggressiveStarvationEnabled*/) const
{
    // TODO(ignat): Support aggressive starvation by starving operation.
    return false;
}

TString TOperationElement::GetLoggingString(const TDynamicAttributesList& dynamicAttributesList) const
{
    return Format(
        "Scheduling info for tree %Qv = {%v, "
        "PreemptableRunningJobs: %v, AggressivelyPreemptableRunningJobs: %v, PreemptionStatusStatistics: %v, DeactivationReasons: %v}",
        GetTreeId(),
        GetLoggingAttributesString(dynamicAttributesList),
        GetPreemptableJobCount(),
        GetAggressivelyPreemptableJobCount(),
        GetPreemptionStatusStatistics(),
        GetDeactivationReasons());
}

bool TOperationElement::ScheduleJob(TFairShareContext* context)
{
    YCHECK(IsActive(context->DynamicAttributesList));

    auto updateAncestorsAttributes = [&] () {
        auto* parent = GetMutableParent();
        while (parent) {
            parent->UpdateDynamicAttributes(context->DynamicAttributesList);
            if (!context->DynamicAttributesList[parent->GetTreeIndex()].Active) {
                ++context->DeactivationReasons[EDeactivationReason::NoBestLeafDescendant];
            }
            parent = parent->GetMutableParent();
        }
    };

    auto disableOperationElement = [&] (EDeactivationReason reason) {
        ++context->DeactivationReasons[reason];
        OnOperationDeactivated(reason);
        context->DynamicAttributesFor(this).Active = false;
        updateAncestorsAttributes();
    };

    auto now = context->SchedulingContext->GetNow();
    if (IsBlocked(now)) {
        disableOperationElement(EDeactivationReason::IsBlocked);
        return false;
    }

    if (!HasJobsSatisfyingResourceLimits(*context)) {
        YT_LOG_TRACE(
            "No pending jobs can satisfy available resources on node "
            "(TreeId: %v, OperationId: %v, FreeResources: %v, DiscountResources: %v)",
            GetTreeId(),
            OperationId_,
            FormatResources(context->SchedulingContext->GetNodeFreeResourcesWithoutDiscount()),
            FormatResources(context->SchedulingContext->ResourceUsageDiscount()));
        disableOperationElement(EDeactivationReason::MinNeededResourcesUnsatisfied);
        return false;
    }

    auto minNeededResources = Controller_->GetAggregatedMinNeededJobResources();
    TJobResources availableResources;

    auto deactivationReason = TryStartScheduleJob(now, minNeededResources, *context, &availableResources);
    if (deactivationReason) {
        disableOperationElement(*deactivationReason);
        return false;
    }
    auto precommittedResources = minNeededResources;

    NProfiling::TWallTimer timer;
    auto scheduleJobResult = DoScheduleJob(context, availableResources, &precommittedResources);
    auto scheduleJobDuration = timer.GetElapsedTime();
    context->TotalScheduleJobDuration += scheduleJobDuration;
    context->ExecScheduleJobDuration += scheduleJobResult->Duration;

    if (!scheduleJobResult->StartDescriptor) {
        for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
            context->FailedScheduleJob[reason] += scheduleJobResult->Failed[reason];
        }

        ++context->ScheduleJobFailureCount;
        disableOperationElement(EDeactivationReason::ScheduleJobFailed);

        bool enableBackoff = scheduleJobResult->IsBackoffNeeded();
        YT_LOG_DEBUG_IF(enableBackoff, "Failed to schedule job, backing off (TreeId: %v, OperationId: %v, Reasons: %v)",
            GetTreeId(),
            OperationId_,
            scheduleJobResult->Failed);

        DecreaseHierarchicalResourceUsagePrecommit(precommittedResources);
        FinishScheduleJob(/*enableBackoff*/ enableBackoff, now);
        return false;
    }

    const auto& startDescriptor = *scheduleJobResult->StartDescriptor;
    if (!OnJobStarted(startDescriptor.Id, startDescriptor.ResourceLimits, precommittedResources)) {
        Controller_->AbortJob(startDescriptor.Id, EAbortReason::SchedulingOperationDisabled);
        disableOperationElement(EDeactivationReason::OperationDisabled);
        DecreaseHierarchicalResourceUsagePrecommit(precommittedResources);
        FinishScheduleJob(/*enableBackoff*/ false, now);
        return false;
    }

    context->SchedulingContext->ResourceUsage() += startDescriptor.ResourceLimits;
    context->SchedulingContext->StartJob(
        GetTreeId(),
        OperationId_,
        scheduleJobResult->IncarnationId,
        startDescriptor);

    UpdateDynamicAttributes(context->DynamicAttributesList);
    updateAncestorsAttributes();

    FinishScheduleJob(/*enableBackoff*/ false, now);
    return true;
}

TString TOperationElement::GetId() const
{
    return ToString(OperationId_);
}

bool TOperationElement::IsAggressiveStarvationPreemptionAllowed() const
{
    return Spec_->AllowAggressiveStarvationPreemption.value_or(true);
}

std::optional<double> TOperationElement::GetSpecifiedWeight() const
{
    return RuntimeParams_->Weight;
}

double TOperationElement::GetMinShareRatio() const
{
    return Spec_->MinShareRatio.value_or(0.0);
}

TJobResources TOperationElement::GetMinShareResources() const
{
    return ToJobResources(Spec_->MinShareResources, ZeroJobResources());
}

double TOperationElement::GetMaxShareRatio() const
{
    return Spec_->MaxShareRatio.value_or(1.0);
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

    if (!starving) {
        LastNonStarvingTime_ = TInstant::Now();
    }

    if (starving && !GetStarving()) {
        SharedState_->ResetDeactivationReasonsFromLastNonStarvingTime();
        TSchedulerElement::SetStarving(true);
        YT_LOG_INFO("Operation is now starving (TreeId: %v, OperationId: %v, Status: %v)",
            GetTreeId(),
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElement::SetStarving(false);
        YT_LOG_INFO("Operation is no longer starving (TreeId: %v, OperationId: %v)",
            GetTreeId(),
            GetId());
    }
}

void TOperationElement::CheckForStarvation(TInstant now)
{
    YCHECK(!Cloned_);

    auto minSharePreemptionTimeout = Attributes_.AdjustedMinSharePreemptionTimeout;
    auto fairSharePreemptionTimeout = Attributes_.AdjustedFairSharePreemptionTimeout;

    double jobCountRatio = GetPendingJobCount() / TreeConfig_->JobCountPreemptionTimeoutCoefficient;

    if (jobCountRatio < 1.0) {
        minSharePreemptionTimeout *= jobCountRatio;
        fairSharePreemptionTimeout *= jobCountRatio;
    }

    TSchedulerElement::CheckForStarvationImpl(
        minSharePreemptionTimeout,
        fairSharePreemptionTimeout,
        now);
}

bool TOperationElement::IsPreemptionAllowed(const TFairShareContext& context, const TFairShareStrategyTreeConfigPtr& config) const
{
    int jobCount = GetRunningJobCount();
    if (jobCount <= config->MaxUnpreemptableRunningJobCount) {
        SharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::ForbiddenSinceLowJobCount);
        return false;
    }

    const TSchedulerElement* element = this;

    while (element && !element->IsRoot()) {
        if (element->GetStarving()) {
            SharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::ForbiddenSinceStarvingParent);
            return false;
        }

        bool aggressivePreemptionEnabled = context.SchedulingStatistics.HasAggressivelyStarvingElements &&
            element->IsAggressiveStarvationPreemptionAllowed() &&
            IsAggressiveStarvationPreemptionAllowed();
        auto threshold = aggressivePreemptionEnabled
            ? config->AggressivePreemptionSatisfactionThreshold
            : config->PreemptionSatisfactionThreshold;

        // NB: we want to use <s>local</s> satisfaction here.
        if (element->ComputeLocalSatisfactionRatio() < threshold + RatioComparisonPrecision) {
            SharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::ForbiddenSinceUnsatisfiedParentOrSelf);
            return false;
        }

        element = element->GetParent();
    }

    SharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::Allowed);
    return true;
}

void TOperationElement::ApplyJobMetricsDelta(const TJobMetrics& delta)
{
    TSchedulerElement* currentElement = this;
    while (currentElement) {
        currentElement->ApplyJobMetricsDeltaLocal(delta);
        currentElement = currentElement->GetMutableParent();
    }
}

void TOperationElement::IncreaseJobResourceUsage(TJobId jobId, const TJobResources& resourcesDelta)
{
    auto delta = SharedState_->IncreaseJobResourceUsage(jobId, resourcesDelta);
    IncreaseHierarchicalResourceUsage(delta);

    UpdatePreemptableJobsList();
}

bool TOperationElement::IsJobKnown(TJobId jobId) const
{
    return SharedState_->IsJobKnown(jobId);
}

bool TOperationElement::IsJobPreemptable(TJobId jobId, bool aggressivePreemptionEnabled) const
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

TPreemptionStatusStatisticsVector TOperationElement::GetPreemptionStatusStatistics() const
{
    return SharedState_->GetPreemptionStatusStatistics();
}

TInstant TOperationElement::GetLastNonStarvingTime() const
{
    return LastNonStarvingTime_;
}

TInstant TOperationElement::GetLastScheduleJobSuccessTime() const
{
    return SharedState_->GetLastScheduleJobSuccessTime();
}

int TOperationElement::GetSlotIndex() const
{
    auto slotIndex = Operation_->FindSlotIndex(GetTreeId());
    YCHECK(slotIndex);
    return *slotIndex;
}

TString TOperationElement::GetUserName() const
{
    return Operation_->GetAuthenticatedUser();
}

bool TOperationElement::OnJobStarted(
    TJobId jobId,
    const TJobResources& resourceUsage,
    const TJobResources& precommittedResources,
    bool force)
{
    // XXX(ignat): remove before deploy on production clusters.
    YT_LOG_DEBUG("Adding job to strategy (JobId: %v)", jobId);

    auto resourceUsageDelta = SharedState_->AddJob(jobId, resourceUsage, force);
    if (resourceUsageDelta) {
        CommitHierarchicalResourceUsage(*resourceUsageDelta, precommittedResources);
        UpdatePreemptableJobsList();
        return true;
    } else {
        return false;
    }
}

void TOperationElement::OnJobFinished(TJobId jobId)
{
    // XXX(ignat): remove before deploy on production clusters.
    YT_LOG_DEBUG("Removing job from strategy (JobId: %v)", jobId);

    auto delta = SharedState_->RemoveJob(jobId);
    if (delta) {
        IncreaseHierarchicalResourceUsage(-(*delta));
        UpdatePreemptableJobsList();
    }
}

void TOperationElement::BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap)
{
    operationElementByIdMap->emplace(OperationId_, this);
}

TSchedulerElementPtr TOperationElement::Clone(TCompositeSchedulerElement* clonedParent)
{
    return New<TOperationElement>(*this, clonedParent);
}

bool TOperationElement::IsSchedulable() const
{
    YCHECK(!Cloned_);

    return Schedulable_;
}

bool TOperationElement::IsBlocked(NProfiling::TCpuInstant now) const
{
    return
        !Schedulable_ ||
        GetPendingJobCount() == 0 ||
        Controller_->IsBlocked(
            now,
            Spec_->MaxConcurrentControllerScheduleJobCalls.value_or(ControllerConfig_->MaxConcurrentControllerScheduleJobCalls),
            ControllerConfig_->ScheduleJobFailBackoffTime);
}

TJobResources TOperationElement::GetHierarchicalAvailableResources(const TFairShareContext& context) const
{
    // Bound available resources with node free resources.
    auto availableResources = context.SchedulingContext->GetNodeFreeResourcesWithDiscount();

    // Bound available resources with pool free resources.
    const TSchedulerElement* parent = this;
    while (parent) {
        availableResources = Min(availableResources, parent->GetLocalAvailableResourceLimits(context));
        parent = parent->GetParent();
    }

    return availableResources;
}

TScheduleJobResultPtr TOperationElement::DoScheduleJob(
    TFairShareContext* context,
    const TJobResources& availableResources,
    TJobResources* precommittedResources)
{
    ++context->SchedulingStatistics.ControllerScheduleJobCount;

    auto scheduleJobResult = Controller_->ScheduleJob(
        context->SchedulingContext,
        availableResources,
        ControllerConfig_->ScheduleJobTimeLimit,
        GetTreeId());

    // Discard the job in case of resource overcommit.
    if (scheduleJobResult->StartDescriptor) {
        const auto& startDescriptor = *scheduleJobResult->StartDescriptor;
        // Note: resourceDelta might be negative.
        const auto resourceDelta = startDescriptor.ResourceLimits - *precommittedResources;
        bool successfullyPrecommitted = TryIncreaseHierarchicalResourceUsagePrecommit(
            resourceDelta,
            *context,
            /* checkDemand */ false,
            /* availableResourceLimitsOutput */ nullptr);
        if (successfullyPrecommitted) {
            *precommittedResources += resourceDelta;
        } else {
            const auto& jobId = scheduleJobResult->StartDescriptor->Id;
            const auto availableDelta = GetHierarchicalAvailableResources(*context);
            YT_LOG_DEBUG("Aborting job with resource overcommit (JobId: %v, OperationId: %v, Limits: %v, JobResources: %v)",
                jobId,
                OperationId_,
                FormatResources(*precommittedResources + availableDelta),
                FormatResources(startDescriptor.ResourceLimits));

            Controller_->AbortJob(jobId, EAbortReason::SchedulingResourceOvercommit);

            // Reset result.
            scheduleJobResult = New<TScheduleJobResult>();
            scheduleJobResult->RecordFail(EScheduleJobFailReason::ResourceOvercommit);
        }
    } else if (scheduleJobResult->Failed[EScheduleJobFailReason::Timeout] > 0) {
        YT_LOG_WARNING("Job scheduling timed out (OperationId: %v)",
            OperationId_);

        SetOperationAlert(
            OperationId_,
            EOperationAlertType::ScheduleJobTimedOut,
            TError("Job scheduling timed out: either scheduler is under heavy load or operation is too heavy"),
            ControllerConfig_->ScheduleJobTimeoutAlertResetTime);
    } else if (scheduleJobResult->Failed[EScheduleJobFailReason::TentativeTreeDeclined] > 0) {
        Controller_->OnTentativeTreeScheduleJobFailed(context->SchedulingContext->GetNow(), TreeId_);
    }

    return scheduleJobResult;
}

TJobResources TOperationElement::ComputeResourceDemand() const
{
    if (Operation_->IsSchedulable()) {
        return GetLocalResourceUsage() + Controller_->GetNeededResources();
    }
    return ZeroJobResources();
}

TJobResources TOperationElement::ComputeResourceLimits() const
{
    return ComputeResourceLimitsBase(RuntimeParams_->ResourceLimits);
}

TJobResources TOperationElement::ComputeMaxPossibleResourceUsage() const
{
    return Min(ResourceLimits(), ResourceDemand());
}

int TOperationElement::ComputePendingJobCount() const
{
    return Controller_->GetPendingJobCount();
}

void TOperationElement::UpdatePreemptableJobsList()
{
    TWallTimer timer;
    int moveCount = 0;

    SharedState_->UpdatePreemptableJobsList(
        GetFairShareRatio(),
        TotalResourceLimits_,
        TreeConfig_->PreemptionSatisfactionThreshold,
        TreeConfig_->AggressivePreemptionSatisfactionThreshold,
        &moveCount);

    auto elapsed = timer.GetElapsedTime();

    Profiler.Update(GetTreeHost()->GetProfilingCounter("/preemptable_list_update_time"), DurationToValue(elapsed));
    Profiler.Update(GetTreeHost()->GetProfilingCounter("/preemptable_list_update_move_count"), moveCount);

    if (elapsed > TreeConfig_->UpdatePreemptableListDurationLoggingThreshold) {
        YT_LOG_DEBUG("Preemptable list update is too long (Duration: %v, MoveCount: %v, OperationId: %v, TreeId: %v)",
            elapsed.MilliSeconds(),
            moveCount,
            OperationId_,
            GetTreeId());
    }
}

bool TOperationElement::TryIncreaseHierarchicalResourceUsagePrecommit(
    const TJobResources& delta,
    const TFairShareContext& context,
    bool checkDemand,
    TJobResources* availableResourceLimitsOutput)
{
    auto availableResourceLimits = InfiniteJobResources();
    TSchedulerElement* failedParent = nullptr;

    if (checkDemand && !CheckDemand(delta, context)) {
        return false;
    }

    TSchedulerElement* currentElement = this;
    while (currentElement) {
        TJobResources localAvailableResourceLimits;
        bool successfullyUpdated = currentElement->TryIncreaseLocalResourceUsagePrecommit(
            delta,
            context,
            &localAvailableResourceLimits);
        if (!successfullyUpdated) {
            failedParent = currentElement;
            break;
        }
        availableResourceLimits = Min(availableResourceLimits, localAvailableResourceLimits);
        currentElement = currentElement->GetMutableParent();
    }

    if (failedParent != nullptr) {
        currentElement = this;
        while (currentElement != failedParent) {
            currentElement->IncreaseLocalResourceUsagePrecommit(-delta);
            currentElement = currentElement->GetMutableParent();
        }
        return false;
    }

    if (availableResourceLimitsOutput != nullptr) {
        *availableResourceLimitsOutput = availableResourceLimits;
    }
    return true;
}

void TOperationElement::DecreaseHierarchicalResourceUsagePrecommit(const TJobResources& precommittedResources)
{
    TSchedulerElement* currentElement = this;
    while (currentElement) {
        currentElement->IncreaseLocalResourceUsagePrecommit(-precommittedResources);
        currentElement = currentElement->GetMutableParent();
    }
}

void TOperationElement::CommitHierarchicalResourceUsage(
    const TJobResources& resourceUsageDelta,
    const TJobResources& precommittedResources)
{
    TSchedulerElement* currentElement = this;
    while (currentElement) {
        currentElement->CommitLocalResourceUsage(resourceUsageDelta, precommittedResources);
        currentElement = currentElement->GetMutableParent();
    }
}

////////////////////////////////////////////////////////////////////////////////

TRootElement::TRootElement(
    ISchedulerStrategyHost* host,
    IFairShareTreeHost* treeHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    NProfiling::TTagId profilingTag,
    const TString& treeId)
    : TCompositeSchedulerElement(
        host,
        treeHost,
        treeConfig,
        profilingTag,
        treeId)
{
    SetFairShareRatio(1.0);
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

void TRootElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    TCompositeSchedulerElement::UpdateTreeConfig(config);

    Attributes_.AdjustedFairShareStarvationTolerance = GetFairShareStarvationTolerance();
    Attributes_.AdjustedMinSharePreemptionTimeout = GetMinSharePreemptionTimeout();
    Attributes_.AdjustedFairSharePreemptionTimeout = GetFairSharePreemptionTimeout();
}

void TRootElement::Update(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(!Cloned_);

    TreeSize_ = TCompositeSchedulerElement::EnumerateElements(0);
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

TString TRootElement::GetId() const
{
    return TString(RootPoolName);
}

std::optional<double> TRootElement::GetSpecifiedWeight() const
{
    return std::nullopt;
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
    return TreeConfig_->FairShareStarvationTolerance;
}

TDuration TRootElement::GetMinSharePreemptionTimeout() const
{
    return TreeConfig_->MinSharePreemptionTimeout;
}

TDuration TRootElement::GetFairSharePreemptionTimeout() const
{
    return TreeConfig_->FairSharePreemptionTimeout;
}

bool TRootElement::IsAggressiveStarvationEnabled() const
{
    return TreeConfig_->EnableAggressiveStarvation;
}

void TRootElement::CheckForStarvation(TInstant now)
{
    Y_UNREACHABLE();
}

int TRootElement::GetMaxRunningOperationCount() const
{
    return TreeConfig_->MaxRunningOperationCount;
}

int TRootElement::GetMaxOperationCount() const
{
    return TreeConfig_->MaxOperationCount;
}

std::vector<EFifoSortParameter> TRootElement::GetFifoSortParameters() const
{
    Y_UNREACHABLE();
}

bool TRootElement::AreImmediateOperationsForbidden() const
{
    return TreeConfig_->ForbidImmediateOperationsInRoot;
}

THashSet<TString> TRootElement::GetAllowedProfilingTags() const
{
    return {};
}

TSchedulerElementPtr TRootElement::Clone(TCompositeSchedulerElement* /*clonedParent*/)
{
    Y_UNREACHABLE();
}

TRootElementPtr TRootElement::Clone()
{
    return New<TRootElement>(*this);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
