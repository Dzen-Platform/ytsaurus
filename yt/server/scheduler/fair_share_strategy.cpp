#include "stdafx.h"
#include "fair_share_strategy.h"
#include "scheduler_strategy.h"
#include "master_connector.h"
#include "job_resources.h"

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;
static auto& Profiler = SchedulerProfiler;

static const double RatioComputationPrecision = 1e-12;
static const double RatioComparisonPrecision = 1e-6;

////////////////////////////////////////////////////////////////////

struct ISchedulerElement;
typedef TIntrusivePtr<ISchedulerElement> ISchedulerElementPtr;

class TOperationElement;
typedef TIntrusivePtr<TOperationElement> TOperationElementPtr;

class TCompositeSchedulerElement;
typedef TIntrusivePtr<TCompositeSchedulerElement> TCompositeSchedulerElementPtr;

class TPool;
typedef TIntrusivePtr<TPool> TPoolPtr;

class TRootElement;
typedef TIntrusivePtr<TRootElement> TRootElementPtr;

////////////////////////////////////////////////////////////////////

struct TSchedulableAttributes
{
    TSchedulableAttributes()
    { }

    EResourceType DominantResource = EResourceType::Cpu;
    double DemandRatio = 0.0;
    double UsageRatio = 0.0;
    double FairShareRatio = 0.0;
    double AdjustedMinShareRatio = 0.0;
    double MaxShareRatio = 1.0;
    double SatisfactionRatio = 0.0;
    double BestAllocationRatio = 1.0;
    i64 DominantLimit = 0;
    bool Active = true;
};

////////////////////////////////////////////////////////////////////

struct ISchedulerElement
    : public TRefCounted
{
    virtual void Update() = 0;
    virtual void UpdateBottomUp() = 0;
    virtual void UpdateTopDown() = 0;

    virtual void BeginHeartbeat() = 0;
    virtual void UpdateSatisfaction() = 0;
    virtual void PrescheduleJob(TExecNodePtr node, bool starvingOnly) = 0;
    virtual bool ScheduleJob(ISchedulingContext* context, bool starvingOnly) = 0;
    virtual void EndHeartbeat() = 0;

    virtual const TSchedulableAttributes& Attributes() const = 0;
    virtual TSchedulableAttributes& Attributes() = 0;
    virtual void UpdateAttributes() = 0;

    virtual TInstant GetStartTime() const = 0;

    virtual Stroka GetId() const = 0;

    virtual double GetWeight() const = 0;
    virtual double GetMinShareRatio() const = 0;
    virtual double GetMaxShareRatio() const = 0;

    virtual const TNodeResources& ResourceDemand() const = 0;
    virtual const TNodeResources& ResourceUsage() const = 0;
    virtual const TNodeResources& ResourceUsageDiscount() const = 0;
    virtual const TNodeResources& ResourceLimits() const = 0;

    virtual void IncreaseUsage(const TNodeResources& delta) = 0;
};

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

template <>
struct THash<NYT::NScheduler::ISchedulerElementPtr> {
    inline size_t operator()(const NYT::NScheduler::ISchedulerElementPtr& a) const {
        return THash<Stroka>()(a->GetId());
    }
};

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TSchedulerElementBase
    : public ISchedulerElement
{
public:
    virtual void Update() override
    {
        UpdateBottomUp();
        UpdateTopDown();
    }

    // Updates attributes that need to be computed from leafs up to root.
    // For example: parent->ResourceDemand = Sum(child->ResourceDemand).
    virtual void UpdateBottomUp() override
    {
        UpdateAttributes();
    }

    // Updates attributes that are propagated from root down to leafs.
    // For example: child->FairShareRatio = fraction(parent->FairShareRatio).
    virtual void UpdateTopDown() override
    { }

    virtual void BeginHeartbeat() override
    {
        Attributes_.Active = true;
    }

    virtual void UpdateSatisfaction() override
    {
        Attributes_.SatisfactionRatio = ComputeLocalSatisfactionRatio();
    }

    virtual void PrescheduleJob(TExecNodePtr /*node*/, bool /*starvingOnly*/) override
    {
        UpdateSatisfaction();
    }

    virtual void EndHeartbeat() override
    { }

    virtual TNullable<Stroka> GetSchedulingTag() const
    {
        return Null;
    }

    virtual void UpdateAttributes() override
    {
        // Choose dominant resource types, compute max share ratios, compute demand ratios.
        auto demand = ResourceDemand();
        auto usage = ResourceUsage() - ResourceUsageDiscount();
        auto totalLimits = Host->GetTotalResourceLimits();
        auto allocationLimits = GetAdjustedResourceLimits(
            demand,
            totalLimits,
            Host->GetExecNodeCount());
        auto limits = Min(totalLimits, ResourceLimits());

        Attributes_.DominantResource = GetDominantResource(usage, totalLimits);

        i64 dominantDemand = GetResource(demand, Attributes_.DominantResource);
        i64 dominantUsage = GetResource(usage, Attributes_.DominantResource);
        i64 dominantAllocationLimit = GetResource(allocationLimits, Attributes_.DominantResource);
        i64 dominantLimit = GetResource(totalLimits, Attributes_.DominantResource);

        Attributes_.DemandRatio =
            dominantLimit == 0 ? 1.0 : (double) dominantDemand / dominantLimit;

        Attributes_.UsageRatio =
            dominantLimit == 0 ? 1.0 : (double) dominantUsage / dominantLimit;

        Attributes_.BestAllocationRatio =
            dominantLimit == 0 ? 1.0 : (double) dominantAllocationLimit / dominantLimit;

        Attributes_.DominantLimit = dominantLimit;

        Attributes_.MaxShareRatio = GetMaxShareRatio();
        if (Attributes_.UsageRatio > RatioComputationPrecision)
        {
            Attributes_.MaxShareRatio = std::min(
                    GetMinResourceRatio(limits, usage) * Attributes_.UsageRatio,
                    Attributes_.MaxShareRatio);
        }
    }

    void IncreaseUsageRatio(const TNodeResources& delta)
    {
        if (Attributes_.DominantLimit != 0) {
            i64 dominantDeltaUsage = GetResource(delta, Attributes_.DominantResource);
            Attributes_.UsageRatio += (double) dominantDeltaUsage / Attributes_.DominantLimit;
        } else {
            Attributes_.UsageRatio = 1.0;
        }
    }

    virtual void IncreaseUsage(const TNodeResources& delta) override
    { }

    DEFINE_BYREF_RW_PROPERTY(TSchedulableAttributes, Attributes);

protected:
    ISchedulerStrategyHost* Host;

    explicit TSchedulerElementBase(ISchedulerStrategyHost* host)
        : Host(host)
    { }

    double ComputeLocalSatisfactionRatio() const
    {
        double minShareRatio = Attributes_.AdjustedMinShareRatio;
        double fairShareRatio = Attributes_.FairShareRatio;
        double usageRatio = Attributes_.UsageRatio;

        // Check for corner cases.
        if (fairShareRatio < RatioComparisonPrecision) {
            return 1.0;
        }

        if (minShareRatio > RatioComparisonPrecision && usageRatio < minShareRatio) {
            // Needy element, negative satisfaction.
            return usageRatio / minShareRatio - 1.0;
        } else {
            // Regular element, positive satisfaction.
            return usageRatio / fairShareRatio;
        }
    }
};

////////////////////////////////////////////////////////////////////

class TCompositeSchedulerElement
    : public TSchedulerElementBase
{
public:
    explicit TCompositeSchedulerElement(ISchedulerStrategyHost* host)
        : TSchedulerElementBase(host)
        , ResourceDemand_(ZeroNodeResources())
        , Mode(ESchedulingMode::Fifo)
    { }

    virtual void UpdateBottomUp() override
    {
        ResourceDemand_ = ZeroNodeResources();
        Attributes_.BestAllocationRatio = 0.0;
        for (const auto& child : Children) {
            child->UpdateBottomUp();

            ResourceDemand_ += child->ResourceDemand();
            Attributes_.BestAllocationRatio = std::max(
                Attributes_.BestAllocationRatio,
                child->Attributes().BestAllocationRatio);
        }
        TSchedulerElementBase::UpdateBottomUp();
    }

    virtual void UpdateTopDown() override
    {
        switch (Mode) {
            case ESchedulingMode::Fifo:
                // Easy case -- the first child get everything, others get none.
                UpdateFifo();
                break;

            case ESchedulingMode::FairShare:
                // Hard case -- compute fair shares using fit factor.
                UpdateFairShare();
                break;

            default:
                YUNREACHABLE();
        }

        // Propagate updates to children.
        for (const auto& child : Children) {
            child->UpdateTopDown();
        }
    }

    virtual void BeginHeartbeat() override
    {
        TSchedulerElementBase::BeginHeartbeat();
        for (const auto& child : Children) {
            child->BeginHeartbeat();
        }
    }

    virtual void UpdateSatisfaction() override
    {
        // Compute local satisfaction ratio.
        Attributes_.SatisfactionRatio = ComputeLocalSatisfactionRatio();
        // Start times bubble up from leaf nodes with operations.
        MinSubtreeStartTime = TInstant::Max();
        // Adjust satisfaction ratio using children.
        // Declare the element passive if all children are passive.
        Attributes_.Active = false;

        for (const auto& child : GetActiveChildren()) {
            if (child->Attributes().Active) {
                // We need to evaluate both MinSubtreeStartTime and SatisfactionRatio
                // because parent can use different scheduling mode.
                MinSubtreeStartTime = std::min(MinSubtreeStartTime, child->GetStartTime());

                Attributes_.SatisfactionRatio = std::min(
                    Attributes_.SatisfactionRatio,
                    child->Attributes().SatisfactionRatio);

                Attributes_.Active = true;
            }
        }
    }

    virtual void PrescheduleJob(TExecNodePtr node, bool starvingOnly) override
    {
        if (!Attributes_.Active)
            return;

        if (!node->CanSchedule(GetSchedulingTag())) {
            Attributes_.Active = false;
            return;
        }

        for (const auto& child : GetActiveChildren()) {
            child->PrescheduleJob(node, starvingOnly);
        }
        UpdateSatisfaction();
    }

    virtual bool ScheduleJob(ISchedulingContext* context, bool starvingOnly) override
    {
        auto bestChild = GetBestChild();

        if (!bestChild) {
            return false;
        }

        // NB: Ignore the child's result.
        bestChild->ScheduleJob(context, starvingOnly);

        return true;
    }

    virtual void EndHeartbeat() override
    {
        TSchedulerElementBase::EndHeartbeat();
        for (const auto& child : Children) {
            child->EndHeartbeat();
        }
    }

    void AddChild(ISchedulerElementPtr child)
    {
        YCHECK(Children.insert(child).second);
    }

    void RemoveChild(ISchedulerElementPtr child)
    {
        YCHECK(Children.erase(child) == 1);
    }

    std::vector<ISchedulerElementPtr> GetChildren() const
    {
        return std::vector<ISchedulerElementPtr>(Children.begin(), Children.end());
    }

    bool IsEmpty() const
    {
        return Children.empty();
    }

    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceDemand);

protected:
    ESchedulingMode Mode;

    yhash_set<ISchedulerElementPtr> Children;

    TInstant MinSubtreeStartTime;

    // Given a non-descending continuous |f|, |f(0) = 0|, and a scalar |a|,
    // computes |x \in [0,1]| s.t. |f(x) = a|.
    // If |f(1) < a| then still returns 1.
    template <class F>
    static double BinarySearch(const F& f, double a)
    {
        if (f(1) < a) {
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
    void ComputeByFitting(
        const TGetter& getter,
        const TSetter& setter,
        double sum)
    {
        auto getSum = [&] (double fitFactor) -> double {
            double sum = 0.0;
            for (const auto& child : Children) {
                sum += getter(fitFactor, child);
            }
            return sum;
        };

        // Run binary search to compute fit factor.
        double fitFactor = BinarySearch(getSum, sum);

        // Compute actual min shares from fit factor.
        for (const auto& child : Children) {
            double value = getter(fitFactor, child);
            setter(child, value);
        }
    }


    void UpdateFifo()
    {
        auto bestChild = GetBestChildFifo(false);
        for (const auto& child : Children) {
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

    void UpdateFairShare()
    {
        // Compute min shares.
        // Compute min weight.
        double minShareSum = 0.0;
        double minWeight = 1.0;
        for (const auto& child : Children) {
            auto& childAttributes = child->Attributes();
            double result = child->GetMinShareRatio();
            // Never give more than demanded.
            result = std::min(result, childAttributes.DemandRatio);
            // Never give more than max share allows.
            result = std::min(result, childAttributes.MaxShareRatio);
            // Never give more than we can allocate.
            result = std::min(result, childAttributes.BestAllocationRatio);
            childAttributes.AdjustedMinShareRatio = result;
            minShareSum += result;

            if (child->GetWeight() > RatioComparisonPrecision) {
                minWeight = std::min(minWeight, child->GetWeight());
            }
        }

        // Normalize min shares, if needed.
        if (minShareSum > Attributes_.AdjustedMinShareRatio) {
            double fitFactor = Attributes_.AdjustedMinShareRatio / minShareSum;
            for (const auto& child : Children) {
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
                // Never give more than demanded.
                result = std::min(result, childAttributes.DemandRatio);
                // Never give more than max share allows.
                result = std::min(result, childAttributes.MaxShareRatio);
                // Never give more than we can allocate.
                result = std::min(result, childAttributes.BestAllocationRatio);
                return result;
            },
            [&] (const ISchedulerElementPtr& child, double value) {
                auto& attributes = child->Attributes();
                attributes.FairShareRatio = value;
            },
            Attributes_.FairShareRatio);
    }


    std::vector<ISchedulerElementPtr> GetActiveChildren() const
    {
        std::vector<ISchedulerElementPtr> result;
        result.reserve(Children.size());
        for (const auto& child : Children) {
            if (child->Attributes().Active) {
                result.push_back(child);
            }
        }
        return result;
    }

    ISchedulerElementPtr GetBestChild() const
    {
        switch (Mode) {
            case ESchedulingMode::Fifo:
                return GetBestChildFifo(true);
            case ESchedulingMode::FairShare:
                return GetBestChildFairShare();
            default:
                YUNREACHABLE();
        }
    }

    ISchedulerElementPtr GetBestChildFifo(bool needsActive) const
    {
        auto isBetter = [] (const ISchedulerElementPtr& lhs, const ISchedulerElementPtr& rhs) -> bool {
            if (lhs->GetWeight() > rhs->GetWeight()) {
                return true;
            }
            if (lhs->GetWeight() < rhs->GetWeight()) {
                return false;
            }
            return lhs->GetStartTime() < rhs->GetStartTime();
        };

        ISchedulerElementPtr bestChild;
        for (const auto& child : Children) {
            if (needsActive && !child->Attributes().Active)
                continue;

            if (bestChild && isBetter(bestChild, child))
                continue;

            bestChild = child;
        }
        return bestChild;
    }

    ISchedulerElementPtr GetBestChildFairShare() const
    {
        ISchedulerElementPtr bestChild;
        for (const auto& child : GetActiveChildren()) {
            if (!bestChild ||
                child->Attributes().SatisfactionRatio < bestChild->Attributes().SatisfactionRatio)
            {
                bestChild = child;
            }
        }
        return bestChild;
    }


    void SetMode(ESchedulingMode mode)
    {
        if (Mode != mode) {
            Mode = mode;
            Update();
        }
    }

};

////////////////////////////////////////////////////////////////////

class TPool
    : public TCompositeSchedulerElement
{
public:
    TPool(
        ISchedulerStrategyHost* host,
        const Stroka& id)
        : TCompositeSchedulerElement(host)
        , Parent_(nullptr)
        , ResourceUsage_(ZeroNodeResources())
        , ResourceUsageDiscount_(ZeroNodeResources())
        , ResourceLimits_(InfiniteNodeResources())
        , Id(id)
    {
        SetDefaultConfig();
    }


    bool IsDefaultConfigured() const
    {
        return DefaultConfigured;
    }

    TPoolConfigPtr GetConfig()
    {
        return Config;
    }

    void SetConfig(TPoolConfigPtr config)
    {
        DoSetConfig(config);
        DefaultConfigured = false;
    }

    void SetDefaultConfig()
    {
        DoSetConfig(New<TPoolConfig>());
        DefaultConfigured = true;
    }

    virtual TInstant GetStartTime() const override
    {
        // For pools StartTime is equal to minimal start time among active children.
        return MinSubtreeStartTime;
    }

    virtual Stroka GetId() const override
    {
        return Id;
    }

    virtual double GetWeight() const override
    {
        return Config->Weight;
    }

    virtual double GetMinShareRatio() const override
    {
        return Config->MinShareRatio;
    }

    virtual double GetMaxShareRatio() const override
    {
        return Config->MaxShareRatio;
    }

    virtual TNullable<Stroka> GetSchedulingTag() const override
    {
        return Config->SchedulingTag;
    }

    virtual void UpdateBottomUp() override
    {
        ResourceLimits_ = ComputeResourceLimits();
        TCompositeSchedulerElement::UpdateBottomUp();
    }

    virtual void IncreaseUsage(const TNodeResources& delta) override
    {
        auto* currentPool = this;
        while (currentPool) {
            currentPool->ResourceUsage() += delta;
            currentPool->IncreaseUsageRatio(delta);
            currentPool->UpdateSatisfaction();
            currentPool = currentPool->GetParent();
        }
    }

    DEFINE_BYVAL_RW_PROPERTY(TPool*, Parent);

    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsageDiscount);
    DEFINE_BYREF_RO_PROPERTY(TNodeResources, ResourceLimits);

private:
    Stroka Id;

    TPoolConfigPtr Config;
    bool DefaultConfigured;


    void DoSetConfig(TPoolConfigPtr newConfig)
    {
        Config = newConfig;
        SetMode(Config->Mode);
    }

    TNodeResources ComputeResourceLimits() const
    {
        auto combinedLimits = Host->GetResourceLimits(GetSchedulingTag()) * Config->MaxShareRatio;
        auto perTypeLimits = Config->ResourceLimits->ToNodeResources();
        return Min(combinedLimits, perTypeLimits);
    }

};

////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EOperationStatus,
    (Normal)
    (BelowMinShare)
    (BelowFairShare)
);

class TOperationElement
    : public TSchedulerElementBase
{
public:
    TOperationElement(
        TFairShareStrategyConfigPtr config,
        TStrategyOperationSpecPtr spec,
        TOperationRuntimeParamsPtr runtimeParams,
        ISchedulerStrategyHost* host,
        TOperationPtr operation)
        : TSchedulerElementBase(host)
        , Operation_(operation)
        , Spec_(spec)
        , RuntimeParams_(runtimeParams)
        , Pool_(nullptr)
        , Starving_(false)
        , ResourceUsage_(ZeroNodeResources())
        , ResourceUsageDiscount_(ZeroNodeResources())
        , NonpreemptableResourceUsage_(ZeroNodeResources())
        , Config(config)
    { }


    virtual void PrescheduleJob(TExecNodePtr node, bool starvingOnly) override
    {
        TSchedulerElementBase::PrescheduleJob(node, starvingOnly);

        if (!node->CanSchedule(GetSchedulingTag())) {
            Attributes_.Active = false;
        }

        if (starvingOnly && !Starving_) {
            Attributes_.Active = false;
        }

        if (Operation_->GetState() != EOperationState::Running) {
            Attributes_.Active = false;
        }
    }

    virtual bool ScheduleJob(ISchedulingContext* context, bool starvingOnly) override
    {
        if (starvingOnly && !Starving_) {
            return false;
        }

        auto node = context->GetNode();
        auto controller = Operation_->GetController();

        // Compute job limits from node limits and pool limits.
        auto jobLimits = node->ResourceLimits() - node->ResourceUsage() + node->ResourceUsageDiscount();
        auto* pool = Pool_;
        while (pool) {
            auto poolLimits = pool->ResourceLimits() - pool->ResourceUsage() + pool->ResourceUsageDiscount();
            jobLimits = Min(jobLimits, poolLimits);
            pool = pool->GetParent();
        }
        auto operationLimits = ResourceLimits() - ResourceUsage();
        jobLimits = Min(jobLimits, operationLimits);

        auto job = controller->ScheduleJob(context, jobLimits);
        if (job) {
            return true;
        } else {
            Attributes_.Active = false;
            auto* pool = Pool_;
            while (pool) {
                pool->UpdateSatisfaction();
                pool = pool->GetParent();
            }
            return false;
        }
    }

    virtual TInstant GetStartTime() const override
    {
        return Operation_->GetStartTime();
    }

    virtual Stroka GetId() const override
    {
        return ToString(Operation_->GetId());
    }

    virtual double GetWeight() const override
    {
        return RuntimeParams_->Weight;
    }

    virtual double GetMinShareRatio() const override
    {
        return Spec_->MinShareRatio;
    }

    virtual double GetMaxShareRatio() const override
    {
        return Spec_->MaxShareRatio;
    }

    virtual TNullable<Stroka> GetSchedulingTag() const override
    {
        return Spec_->SchedulingTag;
    }

    virtual const TNodeResources& ResourceDemand() const override
    {
        ResourceDemand_ = ZeroNodeResources();
        if (!Operation_->GetSuspended()) {
            auto controller = Operation_->GetController();
            ResourceDemand_ = ResourceUsage_ + controller->GetNeededResources();
        }
        return ResourceDemand_;
    }

    virtual const TNodeResources& ResourceLimits() const override
    {
        ResourceLimits_ = Host->GetResourceLimits(GetSchedulingTag());

        auto perTypeLimits = Spec_->ResourceLimits->ToNodeResources();
        ResourceLimits_ = Min(ResourceLimits_, perTypeLimits);

        return ResourceLimits_;
    }

    EOperationStatus GetStatus() const
    {
        if (Operation_->GetState() != EOperationState::Running) {
            return EOperationStatus::Normal;
        }

        auto controller = Operation_->GetController();
        if (controller->GetPendingJobCount() == 0) {
            return EOperationStatus::Normal;
        }

        double usageRatio = Attributes().UsageRatio;
        double demandRatio = Attributes().DemandRatio;

        double tolerance =
            demandRatio < Attributes_.FairShareRatio + RatioComparisonPrecision
            ? 1.0
            : Spec_->FairShareStarvationTolerance.Get(Config->FairShareStarvationTolerance);

        if (usageRatio > Attributes_.FairShareRatio * tolerance - RatioComparisonPrecision) {
            return EOperationStatus::Normal;
        }

        return usageRatio < Attributes_.AdjustedMinShareRatio
               ? EOperationStatus::BelowMinShare
               : EOperationStatus::BelowFairShare;
    }

    virtual void IncreaseUsage(const TNodeResources& delta) override
    {
        ResourceUsage() += delta;
        IncreaseUsageRatio(delta);
        UpdateSatisfaction();
        GetPool()->IncreaseUsage(delta);
    }

    DEFINE_BYVAL_RO_PROPERTY(TOperationPtr, Operation);
    DEFINE_BYVAL_RO_PROPERTY(TStrategyOperationSpecPtr, Spec);
    DEFINE_BYVAL_RO_PROPERTY(TOperationRuntimeParamsPtr, RuntimeParams);
    DEFINE_BYVAL_RW_PROPERTY(TPool*, Pool);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, BelowMinShareSince);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, BelowFairShareSince);
    DEFINE_BYVAL_RW_PROPERTY(bool, Starving);
    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsageDiscount);

    DEFINE_BYREF_RW_PROPERTY(TNodeResources, NonpreemptableResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TJobList, NonpreemptableJobs);
    DEFINE_BYREF_RW_PROPERTY(TJobList, PreemptableJobs);

private:
    mutable TNodeResources ResourceDemand_;
    mutable TNodeResources ResourceLimits_;

    TFairShareStrategyConfigPtr Config;
};

////////////////////////////////////////////////////////////////////

class TRootElement
    : public TCompositeSchedulerElement
{
public:
    explicit TRootElement(ISchedulerStrategyHost* host)
        : TCompositeSchedulerElement(host)
    {
        Attributes_.FairShareRatio = 1.0;
        Attributes_.AdjustedMinShareRatio = 1.0;
        SetMode(ESchedulingMode::FairShare);
    }

    virtual TInstant GetStartTime() const override
    {
        // For pools StartTime is equal to minimal start time among active children.
        return MinSubtreeStartTime;
    }

    virtual Stroka GetId() const override
    {
        return Stroka("<Root>");
    }

    virtual double GetWeight() const override
    {
        return 1.0;
    }

    virtual double GetMinShareRatio() const override
    {
        return 0.0;
    }

    virtual double GetMaxShareRatio() const override
    {
        return 1.0;
    }

    virtual TNullable<Stroka> GetSchedulingTag() const override
    {
        return Null;
    }

    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TNodeResources, ResourceUsageDiscount);
    DEFINE_BYREF_RO_PROPERTY(TNodeResources, ResourceLimits);
};

////////////////////////////////////////////////////////////////////

class TFairShareStrategy
    : public ISchedulerStrategy
{
public:
    TFairShareStrategy(
        TFairShareStrategyConfigPtr config,
        ISchedulerStrategyHost* host)
        : Config(config)
        , Host(host)
    {
        Host->SubscribeOperationRegistered(BIND(&TFairShareStrategy::OnOperationRegistered, this));
        Host->SubscribeOperationUnregistered(BIND(&TFairShareStrategy::OnOperationUnregistered, this));

        Host->SubscribeJobStarted(BIND(&TFairShareStrategy::OnJobStarted, this));
        Host->SubscribeJobFinished(BIND(&TFairShareStrategy::OnJobFinished, this));
        Host->SubscribeJobUpdated(BIND(&TFairShareStrategy::OnJobUpdated, this));
        Host->SubscribePoolsUpdated(BIND(&TFairShareStrategy::OnPoolsUpdated, this));

        Host->SubscribeOperationRuntimeParamsUpdated(
            BIND(&TFairShareStrategy::OnOperationRuntimeParamsUpdated, this));

        RootElement = New<TRootElement>(Host);
    }


    virtual void ScheduleJobs(ISchedulingContext* context) override
    {
        auto now = TInstant::Now();
        auto node = context->GetNode();

        // Run periodic update.
        if (!LastUpdateTime || now > LastUpdateTime.Get() + Config->FairShareUpdatePeriod) {
            PROFILE_TIMING ("/fair_share_update_time") {
                // The root element get the whole cluster.
                RootElement->Update();
            }
            LastUpdateTime = now;
        }

        // Run periodic logging.
        if (!LastLogTime || now > LastLogTime.Get() + Config->FairShareLogPeriod) {
            // Log pools information.
            Host->LogEventFluently(ELogEventType::FairShareInfo)
                .Do(BIND(&TFairShareStrategy::BuildPoolsInformation, this))
                .Item("operations").DoMapFor(OperationToElement, [=] (TFluentMap fluent, const TOperationMap::value_type& pair) {
                    auto operation = pair.first;
                    BuildYsonMapFluently(fluent)
                        .Item(ToString(operation->GetId()))
                        .BeginMap()
                            .Do(BIND(&TFairShareStrategy::BuildOperationProgress, this, operation))
                        .EndMap();
                });
            LastLogTime = now;
        }

        // Update starvation flags for all operations.
        for (const auto& pair : OperationToElement) {
            CheckForStarvation(pair.second);
        }

        RootElement->BeginHeartbeat();

        // First-chance scheduling.
        LOG_DEBUG("Scheduling new jobs");
        RootElement->PrescheduleJob(context->GetNode(), false);
        while (context->CanStartMoreJobs()) {
            if (!RootElement->ScheduleJob(context, false)) {
                break;
            }
        }

        // Compute discount to node usage.
        LOG_DEBUG("Looking for preemptable jobs");
        yhash_set<TOperationElementPtr> discountedOperations;
        yhash_set<TPoolPtr> discountedPools;
        std::vector<TJobPtr> preemptableJobs;
        for (const auto& job : context->RunningJobs()) {
            auto operation = job->GetOperation();
            auto operationElement = GetOperationElement(operation);
            operationElement->ResourceUsageDiscount() += job->ResourceUsage();
            discountedOperations.insert(operationElement);
            if (IsJobPreemptable(job)) {
                auto* pool = operationElement->GetPool();
                while (pool) {
                    discountedPools.insert(pool);
                    pool->ResourceUsageDiscount() += job->ResourceUsage();
                    pool = pool->GetParent();
                }
                node->ResourceUsageDiscount() += job->ResourceUsage();
                preemptableJobs.push_back(job);
                LOG_DEBUG("Job is preemptable (JobId: %v)",
                    job->GetId());
            }
        }

        RootElement->BeginHeartbeat();

        auto jobsBeforePreemption = context->StartedJobs().size();

        // Second-chance scheduling.
        // NB: Schedule at most one job.
        LOG_DEBUG("Scheduling new jobs with preemption");
        RootElement->PrescheduleJob(context->GetNode(), true);
        while (context->CanStartMoreJobs()) {
            if (!RootElement->ScheduleJob(context, true)) {
                break;
            }
            if (context->StartedJobs().size() != jobsBeforePreemption) {
                break;
            }
        }

        // Reset discounts.
        node->ResourceUsageDiscount() = ZeroNodeResources();
        for (const auto& operationElement : discountedOperations) {
            operationElement->ResourceUsageDiscount() = ZeroNodeResources();
        }
        for (const auto& pool : discountedPools) {
            pool->ResourceUsageDiscount() = ZeroNodeResources();
        }

        // Preempt jobs if needed.
        std::sort(
            preemptableJobs.begin(),
            preemptableJobs.end(),
            [] (const TJobPtr& lhs, const TJobPtr& rhs) {
                return lhs->GetStartTime() > rhs->GetStartTime();
            });

        auto poolLimitsViolated = [&] (TJobPtr job) -> bool {
            auto operation = job->GetOperation();
            auto operationElement = GetOperationElement(operation);
            auto* pool = operationElement->GetPool();
            while (pool) {
                if (!Dominates(pool->ResourceLimits(), pool->ResourceUsage())) {
                    return true;
                }
                pool = pool->GetParent();
            }
            return false;
        };

        auto anyPoolLimitsViolated = [&] () -> bool {
            for (const auto& job : context->StartedJobs()) {
                if (poolLimitsViolated(job)) {
                    return true;
                }
            }
            return false;
        };

        bool nodeLimitsViolated = true;
        bool poolsLimitsViolated = true;

        for (const auto& job : preemptableJobs) {
            // Update flags only if violation is not resolved yet to avoid costly computations.
            if (nodeLimitsViolated) {
                nodeLimitsViolated = !Dominates(node->ResourceLimits(), node->ResourceUsage());
            }
            if (!nodeLimitsViolated && poolsLimitsViolated) {
                poolsLimitsViolated = anyPoolLimitsViolated();
            }

            if (!nodeLimitsViolated && !poolsLimitsViolated) {
                break;
            }

            if (nodeLimitsViolated || (poolsLimitsViolated && poolLimitsViolated(job))) {
                context->PreemptJob(job);
            }
        }

        RootElement->EndHeartbeat();
    }

    virtual void BuildOperationAttributes(TOperationPtr operation, IYsonConsumer* consumer) override
    {
        auto element = GetOperationElement(operation);
        auto serializedParams = ConvertToAttributes(element->GetRuntimeParams());
        BuildYsonMapFluently(consumer)
            .Items(*serializedParams);
    }

    virtual void BuildOperationProgress(TOperationPtr operation, IYsonConsumer* consumer) override
    {
        auto element = GetOperationElement(operation);
        auto pool = element->GetPool();
        BuildYsonMapFluently(consumer)
            .Item("pool").Value(pool->GetId())
            .Item("start_time").Value(element->GetStartTime())
            .Item("scheduling_status").Value(element->GetStatus())
            .Item("starving").Value(element->GetStarving())
            .Item("preemptable_job_count").Value(element->PreemptableJobs().size())
            .Do(BIND(&TFairShareStrategy::BuildElementYson, pool, element));
    }

    virtual void BuildBriefOperationProgress(TOperationPtr operation, IYsonConsumer* consumer) override
    {
        auto element = GetOperationElement(operation);
        auto pool = element->GetPool();
        const auto& attributes = element->Attributes();
        BuildYsonMapFluently(consumer)
            .Item("pool").Value(pool->GetId())
            .Item("fair_share_ratio").Value(attributes.FairShareRatio);
    }

    virtual Stroka GetOperationLoggingProgress(TOperationPtr operation) override
    {
        auto element = GetOperationElement(operation);
        const auto& attributes = element->Attributes();
        return Format(
            "Scheduling = {Status: %v, DominantResource: %v, Demand: %.4lf, "
            "Usage: %.4lf, FairShare: %.4lf, Satisfaction: %.4lf, AdjustedMinShare: %.4lf, "
            "MaxShare: %.4lf,  BestAllocation: %.4lf, "
            "Starving: %v, Weight: %v, "
            "PreemptableRunningJobs: %v}",
            element->GetStatus(),
            attributes.DominantResource,
            attributes.DemandRatio,
            attributes.UsageRatio,
            attributes.FairShareRatio,
            attributes.SatisfactionRatio,
            attributes.AdjustedMinShareRatio,
            attributes.MaxShareRatio,
            attributes.BestAllocationRatio,
            element->GetStarving(),
            element->GetWeight(),
            element->PreemptableJobs().size());
    }

    void BuildPoolsInformation(IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item("pools").DoMapFor(Pools, [&] (TFluentMap fluent, const TPoolMap::value_type& pair) {
                const auto& id = pair.first;
                auto pool = pair.second;
                auto config = pool->GetConfig();
                fluent
                    .Item(id).BeginMap()
                        .Item("mode").Value(config->Mode)
                        .DoIf(pool->GetParent(), [&] (TFluentMap fluent) {
                            fluent
                                .Item("parent").Value(pool->GetParent()->GetId());
                        })
                        .Do(BIND(&TFairShareStrategy::BuildElementYson, RootElement, pool))
                    .EndMap();
            });
    }

    virtual void BuildOrchid(IYsonConsumer* consumer) override
    {
        BuildPoolsInformation(consumer);
    }

    virtual void BuildBriefSpec(TOperationPtr operation, IYsonConsumer* consumer) override
    {
        auto element = GetOperationElement(operation);
        BuildYsonMapFluently(consumer)
            .Item("pool").Value(element->GetPool()->GetId());
    }

private:
    TFairShareStrategyConfigPtr Config;
    ISchedulerStrategyHost* Host;

    typedef yhash_map<Stroka, TPoolPtr> TPoolMap;
    TPoolMap Pools;

    typedef yhash_map<TOperationPtr, TOperationElementPtr> TOperationMap;
    TOperationMap OperationToElement;

    typedef std::list<TJobPtr> TJobList;
    TJobList JobList;
    yhash_map<TJobPtr, TJobList::iterator> JobToIterator;

    TRootElementPtr RootElement;
    TNullable<TInstant> LastUpdateTime;
    TNullable<TInstant> LastLogTime;

    bool IsJobPreemptable(TJobPtr job)
    {
        auto operation = job->GetOperation();
        if (operation->GetState() != EOperationState::Running) {
            return false;
        }

        auto element = GetOperationElement(operation);
        auto spec = element->GetSpec();

        double usageRatio = element->Attributes().UsageRatio;
        if (usageRatio < Config->MinPreemptableRatio) {
            return false;
        }

        const auto& attributes = element->Attributes();
        if (usageRatio < attributes.FairShareRatio) {
            return false;
        }

        if (!job->GetPreemptable()) {
            return false;
        }

        return true;
    }


    TStrategyOperationSpecPtr ParseSpec(TOperationPtr operation, INodePtr specNode)
    {
        try {
            return ConvertTo<TStrategyOperationSpecPtr>(specNode);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error parsing spec of pooled operation %v, defaults will be used",
                operation->GetId());
            return New<TStrategyOperationSpec>();
        }
    }

    TOperationRuntimeParamsPtr BuildInitialRuntimeParams(TStrategyOperationSpecPtr spec)
    {
        auto params = New<TOperationRuntimeParams>();
        params->Weight = spec->Weight;
        return params;
    }


    void OnOperationRegistered(TOperationPtr operation)
    {
        auto spec = ParseSpec(operation, operation->GetSpec());
        auto params = BuildInitialRuntimeParams(spec);

        auto poolId = spec->Pool ? *spec->Pool : operation->GetAuthenticatedUser();
        auto pool = FindPool(poolId);
        if (!pool) {
            pool = New<TPool>(Host, poolId);
            RegisterPool(pool);
        }

        auto operationElement = New<TOperationElement>(
            Config,
            spec,
            params,
            Host,
            operation);
        YCHECK(OperationToElement.insert(std::make_pair(operation, operationElement)).second);

        operationElement->SetPool(pool.Get());
        pool->AddChild(operationElement);
        pool->IncreaseUsage(operationElement->ResourceUsage());

        LOG_INFO("Operation added to pool (OperationId: %v, Pool: %v)",
            operation->GetId(),
            pool->GetId());
    }

    void OnOperationUnregistered(TOperationPtr operation)
    {
        auto operationElement = GetOperationElement(operation);
        auto* pool = operationElement->GetPool();

        YCHECK(OperationToElement.erase(operation) == 1);
        pool->RemoveChild(operationElement);
        pool->IncreaseUsage(-operationElement->ResourceUsage());

        LOG_INFO("Operation removed from pool (OperationId: %v, Pool: %v)",
            operation->GetId(),
            pool->GetId());

        if (pool->IsEmpty() && pool->IsDefaultConfigured()) {
            UnregisterPool(pool);
        }
    }

    void OnOperationRuntimeParamsUpdated(
        TOperationPtr operation,
        INodePtr update)
    {
        auto element = FindOperationElement(operation);
        if (!element)
            return;

        NLogging::TLogger Logger(SchedulerLogger);
        Logger.AddTag("OperationId: %v", operation->GetId());

        try {
            if (ReconfigureYsonSerializable(element->GetRuntimeParams(), update)) {
                LOG_INFO("Operation runtime parameters updated");
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error parsing operation runtime parameters");
        }
    }


    void OnJobStarted(TJobPtr job)
    {
        auto element = GetOperationElement(job->GetOperation());

        auto it = JobList.insert(JobList.begin(), job);
        YCHECK(JobToIterator.insert(std::make_pair(job, it)).second);

        job->SetPreemptable(true);
        element->PreemptableJobs().push_back(job);
        job->SetJobListIterator(--element->PreemptableJobs().end());

        OnJobResourceUsageUpdated(job, element, job->ResourceUsage());
    }

    void OnJobFinished(TJobPtr job)
    {
        auto element = GetOperationElement(job->GetOperation());

        auto it = JobToIterator.find(job);
        YASSERT(it != JobToIterator.end());

        JobList.erase(it->second);
        JobToIterator.erase(it);

        if (job->GetPreemptable()) {
            element->PreemptableJobs().erase(job->GetJobListIterator());
        } else {
            element->NonpreemptableJobs().erase(job->GetJobListIterator());
        }

        OnJobResourceUsageUpdated(job, element, -job->ResourceUsage());
    }

    void OnJobUpdated(TJobPtr job, const TNodeResources& resourcesDelta)
    {
        auto element = GetOperationElement(job->GetOperation());
        OnJobResourceUsageUpdated(job, element, resourcesDelta);
    }


    TCompositeSchedulerElementPtr GetPoolParentElement(TPoolPtr pool)
    {
        auto* parentPool = pool->GetParent();
        return parentPool ? TCompositeSchedulerElementPtr(parentPool) : RootElement;
    }

    // Handles nullptr (aka "root") properly.
    Stroka GetPoolId(TPoolPtr pool)
    {
        return pool ? pool->GetId() : Stroka("<Root>");
    }


    void RegisterPool(TPoolPtr pool)
    {
        YCHECK(Pools.insert(std::make_pair(pool->GetId(), pool)).second);
        GetPoolParentElement(pool)->AddChild(pool);

        LOG_INFO("Pool registered (Pool: %v, Parent: %v)",
            GetPoolId(pool),
            GetPoolId(pool->GetParent()));
    }

    void UnregisterPool(TPoolPtr pool)
    {
        YCHECK(Pools.erase(pool->GetId()) == 1);
        SetPoolParent(pool, nullptr);
        GetPoolParentElement(pool)->RemoveChild(pool);

        LOG_INFO("Pool unregistered (Pool: %v, Parent: %v)",
            GetPoolId(pool),
            GetPoolId(pool->GetParent()));
    }

    void SetPoolParent(TPoolPtr pool, TPoolPtr parent)
    {
        if (pool->GetParent() == parent)
            return;

        auto* oldParent = pool->GetParent();
        if (oldParent) {
            oldParent->IncreaseUsage(-pool->ResourceUsage());
        }
        GetPoolParentElement(pool)->RemoveChild(pool);

        pool->SetParent(parent.Get());

        GetPoolParentElement(pool)->AddChild(pool);
        if (parent) {
            parent->IncreaseUsage(pool->ResourceUsage());
        }
    }

    TPoolPtr FindPool(const Stroka& id)
    {
        auto it = Pools.find(id);
        return it == Pools.end() ? nullptr : it->second;
    }

    TPoolPtr GetPool(const Stroka& id)
    {
        auto pool = FindPool(id);
        YCHECK(pool);
        return pool;
    }


    TOperationElementPtr FindOperationElement(TOperationPtr operation)
    {
        auto it = OperationToElement.find(operation);
        return it == OperationToElement.end() ? nullptr : it->second;
    }

    TOperationElementPtr GetOperationElement(TOperationPtr operation)
    {
        auto element = FindOperationElement(operation);
        YCHECK(element);
        return element;
    }

    void OnPoolsUpdated(INodePtr poolsNode)
    {
        try {
            // Build the set of potential orphans.
            yhash_set<Stroka> orphanPoolIds;
            for (const auto& pair : Pools) {
                YCHECK(orphanPoolIds.insert(pair.first).second);
            }

            // Track ids appearing in various branches of the tree.
            yhash_map<Stroka, TYPath> poolIdToPath;

            // NB: std::function is needed by parseConfig to capture itself.
            std::function<void(INodePtr, TPoolPtr)> parseConfig =
                [&] (INodePtr configNode, TPoolPtr parent) {
                    auto configMap = configNode->AsMap();
                    for (const auto& pair : configMap->GetChildren()) {
                        const auto& childId = pair.first;
                        const auto& childNode = pair.second;
                        auto childPath = childNode->GetPath();
                        if (!poolIdToPath.insert(std::make_pair(childId, childPath)).second) {
                            LOG_ERROR("Pool %Qv is defined both at %v and %v; skipping second occurrence",
                                childId,
                                poolIdToPath[childId],
                                childPath);
                            continue;
                        }

                        // Parse config.
                        auto configNode = ConvertToNode(childNode->Attributes());
                        TPoolConfigPtr config;
                        try {
                            config = ConvertTo<TPoolConfigPtr>(configNode);
                        } catch (const std::exception& ex) {
                            LOG_ERROR(ex, "Error parsing configuration of pool %Qv; using defaults",
                                childPath);
                            config = New<TPoolConfig>();
                        }

                        auto pool = FindPool(childId);
                        if (pool) {
                            // Reconfigure existing pool.
                            pool->SetConfig(config);
                            YCHECK(orphanPoolIds.erase(childId) == 1);
                        } else {
                            // Create new pool.
                            pool = New<TPool>(Host, childId);
                            pool->SetConfig(config);
                            RegisterPool(pool);
                        }
                        SetPoolParent(pool, parent);

                        // Parse children.
                        parseConfig(childNode, pool.Get());
                    }
                };

            // Run recursive descent parsing.
            parseConfig(poolsNode, nullptr);

            // Unregister orphan pools.
            for (const auto& id : orphanPoolIds) {
                auto pool = GetPool(id);
                if (pool->IsEmpty()) {
                    UnregisterPool(pool);
                } else {
                    pool->SetDefaultConfig();
                    SetPoolParent(pool, nullptr);
                }
            }

            RootElement->Update();

            LOG_INFO("Pools updated");
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error updating pools");
        }
    }

    void CheckForStarvation(TOperationElementPtr element)
    {
        auto status = element->GetStatus();
        auto now = TInstant::Now();
        auto spec = element->GetSpec();
        auto minSharePreemptionTimeout = spec->MinSharePreemptionTimeout.Get(Config->MinSharePreemptionTimeout);
        auto fairSharePreemptionTimeout = spec->FairSharePreemptionTimeout.Get(Config->FairSharePreemptionTimeout);
        switch (status) {
            case EOperationStatus::BelowMinShare:
                if (!element->GetBelowMinShareSince()) {
                    element->SetBelowMinShareSince(now);
                } else if (element->GetBelowMinShareSince().Get() < now - minSharePreemptionTimeout) {
                    SetStarving(element, status);
                }
                break;

            case EOperationStatus::BelowFairShare:
                if (!element->GetBelowFairShareSince()) {
                    element->SetBelowFairShareSince(now);
                } else if (element->GetBelowFairShareSince().Get() < now - fairSharePreemptionTimeout) {
                    SetStarving(element, status);
                }
                element->SetBelowMinShareSince(Null);
                break;

            case EOperationStatus::Normal:
                element->SetBelowMinShareSince(Null);
                element->SetBelowFairShareSince(Null);
                ResetStarving(element);
                break;

            default:
                YUNREACHABLE();
        }
    }

    void SetStarving(TOperationElementPtr element, EOperationStatus status)
    {
        if (!element->GetStarving()) {
            element->SetStarving(true);
            LOG_INFO("Operation starvation timeout (OperationId: %v, Status: %v)",
                element->GetOperation()->GetId(),
                status);
        }
    }

    void ResetStarving(TOperationElementPtr element)
    {
        if (element->GetStarving()) {
            element->SetStarving(false);
            LOG_INFO("Operation is no longer starving (OperationId: %v)",
                element->GetOperation()->GetId());
        }
    }


    void OnJobResourceUsageUpdated(
        TJobPtr job,
        TOperationElementPtr element,
        const TNodeResources& resourcesDelta)
    {
        element->IncreaseUsage(resourcesDelta);

        const auto& attributes = element->Attributes();
        auto limits = Host->GetTotalResourceLimits();

        auto& preemptableJobs = element->PreemptableJobs();
        auto& nonpreemptableJobs = element->NonpreemptableJobs();
        auto& nonpreemptableResourceUsage = element->NonpreemptableResourceUsage();

        if (!job->GetPreemptable()) {
            nonpreemptableResourceUsage += resourcesDelta;
        }

        auto getNonpreemptableUsageRatio = [&] (const TNodeResources& extraResources) -> double {
            i64 usage = GetResource(
                nonpreemptableResourceUsage + extraResources,
                attributes.DominantResource);
            i64 limit = GetResource(limits, attributes.DominantResource);
            return limit == 0 ? 1.0 : (double) usage / limit;
        };

        // Remove nonpreemptable jobs exceeding the fair share.
        while (!nonpreemptableJobs.empty()) {
            if (getNonpreemptableUsageRatio(ZeroNodeResources()) <= attributes.FairShareRatio)
                break;

            auto job = nonpreemptableJobs.back();
            YCHECK(!job->GetPreemptable());

            nonpreemptableJobs.pop_back();
            nonpreemptableResourceUsage -= job->ResourceUsage();

            preemptableJobs.push_front(job);

            job->SetPreemptable(true);
            job->SetJobListIterator(preemptableJobs.begin());
        }

        // Add more nonpreemptable jobs until filling up the fair share.
        while (!preemptableJobs.empty()) {
            auto job = preemptableJobs.front();
            YCHECK(job->GetPreemptable());

            if (getNonpreemptableUsageRatio(job->ResourceUsage()) > attributes.FairShareRatio)
                break;

            preemptableJobs.pop_front();

            nonpreemptableJobs.push_back(job);
            nonpreemptableResourceUsage += job->ResourceUsage();

            job->SetPreemptable(false);
            job->SetJobListIterator(--nonpreemptableJobs.end());
        }
    }


    static void BuildElementYson(
        TCompositeSchedulerElementPtr composite,
        ISchedulerElementPtr element,
        IYsonConsumer* consumer)
    {
        const auto& attributes = element->Attributes();
        BuildYsonMapFluently(consumer)
            .Item("resource_demand").Value(element->ResourceDemand())
            .Item("resource_usage").Value(element->ResourceUsage())
            .Item("resource_limits").Value(element->ResourceLimits())
            .Item("dominant_resource").Value(attributes.DominantResource)
            .Item("weight").Value(element->GetWeight())
            .Item("min_share_ratio").Value(element->GetMinShareRatio())
            .Item("adjusted_min_share_ratio").Value(attributes.AdjustedMinShareRatio)
            .Item("max_share_ratio").Value(attributes.MaxShareRatio)
            .Item("usage_ratio").Value(attributes.UsageRatio)
            .Item("demand_ratio").Value(attributes.DemandRatio)
            .Item("fair_share_ratio").Value(attributes.FairShareRatio)
            .Item("satisfaction_ratio").Value(attributes.SatisfactionRatio)
            .Item("best_allocation_ratio").Value(attributes.BestAllocationRatio);
    }

};

std::unique_ptr<ISchedulerStrategy> CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host)
{
    return std::unique_ptr<ISchedulerStrategy>(new TFairShareStrategy(config, host));
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

