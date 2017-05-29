#include "private.h"
#include "config.h"
#include "job.h"
#include "job_resources.h"
#include "scheduler_strategy.h"
#include "scheduling_tag.h"

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EDeactivationReason,
    (IsNotAlive)
    (UnmatchedSchedulingTag)
    (IsNotStarving)
    (IsBlocked)
    (TryStartScheduleJobFailed)
    (ScheduleJobFailed)
    (NoBestLeafDescendant)
);

////////////////////////////////////////////////////////////////////////////////

struct TSchedulableAttributes
{
    NNodeTrackerClient::EResourceType DominantResource = NNodeTrackerClient::EResourceType::Cpu;
    double DemandRatio = 0.0;
    double FairShareRatio = 0.0;
    double AdjustedMinShareRatio = 0.0;
    double RecursiveMinShareRatio = 0.0;
    double MaxPossibleUsageRatio = 1.0;
    double BestAllocationRatio = 1.0;
    double GuaranteedResourcesRatio = 0.0;
    double DominantLimit = 0;
    int FifoIndex = 0;

    double AdjustedFairShareStarvationTolerance = 1.0;
    TDuration AdjustedMinSharePreemptionTimeout;
    TDuration AdjustedFairSharePreemptionTimeout;
};

struct TDynamicAttributes
{
    double SatisfactionRatio = 0.0;
    bool Active = false;
    TSchedulerElement* BestLeafDescendant = nullptr;
    TJobResources ResourceUsageDiscount = ZeroJobResources();
};

typedef std::vector<TDynamicAttributes> TDynamicAttributesList;

////////////////////////////////////////////////////////////////////////////////

struct TFairShareContext
{
    TFairShareContext(
        const ISchedulingContextPtr& schedulingContext,
        int treeSize,
        const std::vector<TSchedulingTagFilter>& filter);

    TDynamicAttributes& DynamicAttributes(TSchedulerElement* element);
    const TDynamicAttributes& DynamicAttributes(TSchedulerElement* element) const;

    std::vector<bool> CanSchedule;
    const ISchedulingContextPtr SchedulingContext;
    TDynamicAttributesList DynamicAttributesList;
    TDuration TotalScheduleJobDuration;
    TDuration ExecScheduleJobDuration;
    TEnumIndexedVector<int, EScheduleJobFailReason> FailedScheduleJob;
    bool HasAggressivelyStarvingNodes = false;

    int ActiveOperationCount = 0;
    int ActiveTreeSize = 0;
    TEnumIndexedVector<int, EDeactivationReason> DeactivationReasons;
};

////////////////////////////////////////////////////////////////////////////////

const int UnassignedTreeIndex = -1;
const int EmptySchedulingTagFilterIndex = -1;

class TSchedulerElementFixedState
{
public:
    DEFINE_BYREF_RO_PROPERTY(TJobResources, ResourceDemand);
    DEFINE_BYREF_RO_PROPERTY(TJobResources, ResourceLimits);
    DEFINE_BYREF_RO_PROPERTY(TJobResources, MaxPossibleResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TSchedulableAttributes, Attributes);
    DEFINE_BYVAL_RW_PROPERTY(int, SchedulingTagFilterIndex, EmptySchedulingTagFilterIndex);

protected:
    explicit TSchedulerElementFixedState(
        ISchedulerStrategyHost* host,
        const TFairShareStrategyConfigPtr& strategyConfig);

    ISchedulerStrategyHost* const Host_;

    TFairShareStrategyConfigPtr StrategyConfig_;

    TCompositeSchedulerElement* Parent_ = nullptr;

    TNullable<TInstant> BelowFairShareSince_;
    bool Starving_ = false;

    TJobResources TotalResourceLimits_;

    int PendingJobCount_ = 0;
    TInstant StartTime_ = TInstant();

    int TreeIndex_ = UnassignedTreeIndex;

    bool Cloned_ = false;

};

class TSchedulerElementSharedState
    : public TIntrinsicRefCounted
{
public:
    TSchedulerElementSharedState();

    TJobResources GetResourceUsage();
    void IncreaseResourceUsage(const TJobResources& delta);

    double GetResourceUsageRatio(NNodeTrackerClient::EResourceType dominantResource, double dominantResourceLimit);

    bool GetAlive() const;
    void SetAlive(bool alive);

private:
    TJobResources ResourceUsage_;
    NConcurrency::TReaderWriterSpinLock ResourceUsageLock_;

    // NB: Avoid false sharing between ResourceUsageLock_ and others.
    char Padding[64];

    std::atomic<bool> Alive_ = {true};

};

DEFINE_REFCOUNTED_TYPE(TSchedulerElementSharedState)

class TSchedulerElement
    : public TSchedulerElementFixedState
    , public TIntrinsicRefCounted
{
public:
    //! Enumerates nodes of the tree using inorder traversal. Returns first unused index.
    virtual int EnumerateNodes(int startIndex);

    int GetTreeIndex() const;

    virtual void UpdateStrategyConfig(const TFairShareStrategyConfigPtr& config);

    virtual void Update(TDynamicAttributesList& dynamicAttributesList);

    //! Updates attributes that need to be computed from leafs up to root.
    //! For example: |parent->ResourceDemand = Sum(child->ResourceDemand)|.
    virtual void UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList);

    //! Updates attributes that are propagated from root down to leafs.
    //! For example: |child->FairShareRatio = fraction(parent->FairShareRatio)|.
    virtual void UpdateTopDown(TDynamicAttributesList& dynamicAttributesList);

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const = 0;

    virtual void UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList);

    virtual void PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled);
    virtual bool ScheduleJob(TFairShareContext& context) = 0;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const;

    bool IsActive(const TDynamicAttributesList& dynamicAttributesList) const;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const = 0;

    bool IsAlive() const;
    void SetAlive(bool alive);

    virtual Stroka GetId() const = 0;

    virtual double GetWeight() const = 0;
    virtual double GetMinShareRatio() const = 0;
    virtual TJobResources GetMinShareResources() const = 0;
    virtual double GetMaxShareRatio() const = 0;

    virtual double GetFairShareStarvationTolerance() const = 0;
    virtual TDuration GetMinSharePreemptionTimeout() const = 0;
    virtual TDuration GetFairSharePreemptionTimeout() const = 0;

    TCompositeSchedulerElement* GetParent() const;
    void SetParent(TCompositeSchedulerElement* parent);

    TInstant GetStartTime() const;
    int GetPendingJobCount() const;

    virtual ESchedulableStatus GetStatus() const;

    bool GetStarving() const;
    virtual void SetStarving(bool starving);
    virtual void CheckForStarvation(TInstant now) = 0;

    TJobResources GetResourceUsage() const;
    double GetResourceUsageRatio() const;

    void IncreaseLocalResourceUsage(const TJobResources& delta);
    virtual void IncreaseResourceUsage(const TJobResources& delta) = 0;

    virtual void BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap) = 0;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) = 0;

protected:
    TSchedulerElementSharedStatePtr SharedState_;

    TSchedulerElement(
        ISchedulerStrategyHost* host,
        const TFairShareStrategyConfigPtr& strategyConfig);
    TSchedulerElement(
        const TSchedulerElement& other,
        TCompositeSchedulerElement* clonedParent);

    ISchedulerStrategyHost* GetHost() const;

    double ComputeLocalSatisfactionRatio() const;

    ESchedulableStatus GetStatus(double defaultTolerance) const;

    void CheckForStarvationImpl(
        TDuration minSharePreemptionTimeout,
        TDuration fairSharePreemptionTimeout,
        TInstant now);

    void UpdateAttributes();

};

DEFINE_REFCOUNTED_TYPE(TSchedulerElement)

////////////////////////////////////////////////////////////////////////////////

class TCompositeSchedulerElementFixedState
{
public:
    DEFINE_BYREF_RW_PROPERTY(int, RunningOperationCount);
    DEFINE_BYREF_RW_PROPERTY(int, OperationCount);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TError>, UpdateFairShareAlerts);

    DEFINE_BYREF_RO_PROPERTY(double, AdjustedFairShareStarvationToleranceLimit);
    DEFINE_BYREF_RO_PROPERTY(TDuration, AdjustedMinSharePreemptionTimeoutLimit);
    DEFINE_BYREF_RO_PROPERTY(TDuration, AdjustedFairSharePreemptionTimeoutLimit);

protected:
    ESchedulingMode Mode_ = ESchedulingMode::Fifo;
    std::vector<EFifoSortParameter> FifoSortParameters_;

};

class TCompositeSchedulerElement
    : public TSchedulerElement
    , public TCompositeSchedulerElementFixedState
{
public:
    TCompositeSchedulerElement(
        ISchedulerStrategyHost* host,
        TFairShareStrategyConfigPtr strategyConfig,
        const Stroka& profilingName);
    TCompositeSchedulerElement(
        const TCompositeSchedulerElement& other,
        TCompositeSchedulerElement* clonedParent);

    virtual int EnumerateNodes(int startIndex) override;

    virtual void UpdateStrategyConfig(const TFairShareStrategyConfigPtr& config) override;

    virtual void UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList) override;
    virtual void UpdateTopDown(TDynamicAttributesList& dynamicAttributesList) override;

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const override;

    virtual double GetFairShareStarvationToleranceLimit() const;
    virtual TDuration GetMinSharePreemptionTimeoutLimit() const;
    virtual TDuration GetFairSharePreemptionTimeoutLimit() const;

    void UpdatePreemptionSettingsLimits();
    void UpdateChildPreemptionSettings(const TSchedulerElementPtr& child);

    virtual void UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList) override;

    virtual void PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled) override;
    virtual bool ScheduleJob(TFairShareContext& context) override;

    virtual void IncreaseResourceUsage(const TJobResources& delta) override;

    virtual bool IsRoot() const;
    virtual bool IsExplicit() const;
    virtual bool IsAggressiveStarvationEnabled() const;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    void AddChild(const TSchedulerElementPtr& child, bool enabled = true);
    void EnableChild(const TSchedulerElementPtr& child);
    void RemoveChild(const TSchedulerElementPtr& child);

    bool IsEmpty() const;

    ESchedulingMode GetMode() const;
    void SetMode(ESchedulingMode);

    NProfiling::TTagId GetProfilingTag() const;

    virtual int GetMaxOperationCount() const = 0;
    virtual int GetMaxRunningOperationCount() const = 0;

    virtual bool AreImmediateOperationsFobidden() const = 0;

    virtual void BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap) override;

protected:
    const NProfiling::TTagId ProfilingTag_;

    using TChildMap = yhash<TSchedulerElementPtr, int>;
    using TChildList = std::vector<TSchedulerElementPtr>;

    TChildMap EnabledChildToIndex_;
    TChildList EnabledChildren_;

    TChildMap DisabledChildToIndex_;
    TChildList DisabledChildren_;

    template <class TGetter, class TSetter>
    void ComputeByFitting(const TGetter& getter, const TSetter& setter, double sum);

    void UpdateFifo(TDynamicAttributesList& dynamicAttributesList);
    void UpdateFairShare(TDynamicAttributesList& dynamicAttributesList);

    TSchedulerElementPtr GetBestActiveChild(const TDynamicAttributesList& dynamicAttributesList) const;
    TSchedulerElementPtr GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const;
    TSchedulerElementPtr GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const;

    static void AddChild(TChildMap* map, TChildList* list, const TSchedulerElementPtr& child);
    static void RemoveChild(TChildMap* map, TChildList* list, const TSchedulerElementPtr& child);
    static bool ContainsChild(const TChildMap& map, const TSchedulerElementPtr& child);

private:
    bool HasHigherPriorityInFifoMode(const TSchedulerElementPtr& lhs, const TSchedulerElementPtr& rhs) const;
};

DEFINE_REFCOUNTED_TYPE(TCompositeSchedulerElement)

////////////////////////////////////////////////////////////////////////////////

class TPoolFixedState
{
protected:
    explicit TPoolFixedState(const Stroka& id);

    const Stroka Id_;
    bool DefaultConfigured_ = true;
    TNullable<Stroka> UserName_;
};

class TPool
    : public TCompositeSchedulerElement
    , public TPoolFixedState
{
public:
    TPool(
        ISchedulerStrategyHost* host,
        const Stroka& id,
        TFairShareStrategyConfigPtr strategyConfig);
    TPool(
        const TPool& other,
        TCompositeSchedulerElement* clonedParent);

    bool IsDefaultConfigured() const;

    void SetUserName(const TNullable<Stroka>& userName);
    const TNullable<Stroka>& GetUserName() const;

    TPoolConfigPtr GetConfig();
    void SetConfig(TPoolConfigPtr config);
    void SetDefaultConfig();

    virtual bool IsExplicit() const override;
    virtual bool IsAggressiveStarvationEnabled() const override;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    virtual Stroka GetId() const override;

    virtual double GetWeight() const override;
    virtual double GetMinShareRatio() const override;
    virtual TJobResources GetMinShareResources() const override;
    virtual double GetMaxShareRatio() const override;

    virtual ESchedulableStatus GetStatus() const override;

    virtual double GetFairShareStarvationTolerance() const override;
    virtual TDuration GetMinSharePreemptionTimeout() const override;
    virtual TDuration GetFairSharePreemptionTimeout() const override;

    virtual double GetFairShareStarvationToleranceLimit() const override;
    virtual TDuration GetMinSharePreemptionTimeoutLimit() const override;
    virtual TDuration GetFairSharePreemptionTimeoutLimit() const override;

    virtual void SetStarving(bool starving) override;
    virtual void CheckForStarvation(TInstant now) override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual void UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList) override;

    virtual int GetMaxRunningOperationCount() const override;
    virtual int GetMaxOperationCount() const override;

    virtual bool AreImmediateOperationsFobidden() const override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;

private:
    TPoolConfigPtr Config_;
    TSchedulingTagFilter SchedulingTagFilter_;

    void DoSetConfig(TPoolConfigPtr newConfig);

    TJobResources ComputeResourceLimits() const;

};

DEFINE_REFCOUNTED_TYPE(TPool)

////////////////////////////////////////////////////////////////////////////////

class TOperationElementFixedState
{
public:
    DEFINE_BYVAL_RO_PROPERTY(IOperationControllerPtr, Controller);

protected:
    explicit TOperationElementFixedState(TOperationPtr operation);

    const TOperationId OperationId_;
    bool Schedulable_;
    TOperation* const Operation_;
};

class TOperationElementSharedState
    : public TIntrinsicRefCounted
{
public:
    TOperationElementSharedState();

    TJobResources IncreaseJobResourceUsage(
        const TJobId& jobId,
        const TJobResources& resourcesDelta);

    void UpdatePreemptableJobsList(
        double fairShareRatio,
        const TJobResources& totalResourceLimits,
        double preemptionSatisfactionThreshold,
        double aggressivePreemptionSatisfactionThreshold);

    bool IsJobExisting(const TJobId& jobId) const;

    bool IsJobPreemptable(const TJobId& jobId, bool aggressivePreemptionEnabled) const;

    int GetRunningJobCount() const;
    int GetPreemptableJobCount() const;
    int GetAggressivelyPreemptableJobCount() const;

    TJobResources AddJob(const TJobId& jobId, const TJobResources resourceUsage);
    TJobResources RemoveJob(const TJobId& jobId);

    bool IsBlocked(
        NProfiling::TCpuInstant now,
        int maxConcurrentScheduleJobCalls,
        NProfiling::TCpuDuration scheduleJobFailBackoffTime) const;

    bool TryStartScheduleJob(
        NProfiling::TCpuInstant now,
        int maxConcurrentScheduleJobCalls,
        NProfiling::TCpuDuration scheduleJobFailBackoffTime);

    void FinishScheduleJob(
        bool enableBackoff,
        NProfiling::TCpuInstant now);

    TJobResources Finalize();

private:
    template <typename T>
    class TListWithSize
    {
    public:
        using iterator = typename std::list<T>::iterator;

        void push_front(const T& value)
        {
            Impl_.push_front(value);
            ++Size_;
        }

        void push_back(const T& value)
        {
            Impl_.push_back(value);
            ++Size_;
        }

        void pop_front()
        {
            Impl_.pop_front();
            --Size_;
        }

        void pop_back()
        {
            Impl_.pop_back();
            --Size_;
        }

        void erase(iterator it)
        {
            Impl_.erase(it);
            --Size_;
        }

        iterator begin()
        {
            return Impl_.begin();
        }

        iterator end()
        {
            return Impl_.end();
        }

        const T& front() const
        {
            return Impl_.front();
        }

        const T& back() const
        {
            return Impl_.back();
        }

        size_t size() const
        {
            return Size_;
        }

        bool empty() const
        {
            return Size_ == 0;
        }

    private:
        std::list<T> Impl_;
        size_t Size_ = 0;

    };

    typedef TListWithSize<TJobId> TJobIdList;

    TJobIdList NonpreemptableJobs_;
    TJobIdList AggressivelyPreemptableJobs_;
    TJobIdList PreemptableJobs_;
    std::atomic<int> RunningJobCount_ = {0};

    TJobResources NonpreemptableResourceUsage_;
    TJobResources AggressivelyPreemptableResourceUsage_;

    struct TJobProperties
    {
        TJobProperties(
            bool preemptable,
            bool aggressivelyPreemptable,
            TJobIdList::iterator jobIdListIterator,
            const TJobResources& resourceUsage)
            : Preemptable(preemptable)
            , AggressivelyPreemptable(aggressivelyPreemptable)
            , JobIdListIterator(jobIdListIterator)
            , ResourceUsage(resourceUsage)
        { }

        //! Determines whether job belongs to list of preemptable or aggressively preemtable jobs of operation.
        bool Preemptable;

        //! Determines whether job belongs to list of preemptable (but not aggressively preemptable) jobs of operation.
        bool AggressivelyPreemptable;

        //! Iterator in the per-operation list pointing to this particular job.
        TJobIdList::iterator JobIdListIterator;

        TJobResources ResourceUsage;

        static void SetPreemptable(TJobProperties* properties)
        {
            properties->Preemptable = true;
            properties->AggressivelyPreemptable = true;
        }

        static void SetAggressivelyPreemptable(TJobProperties* properties)
        {
            properties->Preemptable = false;
            properties->AggressivelyPreemptable = true;
        }

        static void SetNonPreemptable(TJobProperties* properties)
        {
            properties->Preemptable = false;
            properties->AggressivelyPreemptable = false;
        }
    };

    yhash<TJobId, TJobProperties> JobPropertiesMap_;
    NConcurrency::TReaderWriterSpinLock JobPropertiesMapLock_;

    std::atomic<int> ConcurrentScheduleJobCalls_ = {0};
    std::atomic<NProfiling::TCpuInstant> LastScheduleJobFailTime_ = {0};

    bool Finalized_ = false;

    NJobTrackerClient::TStatistics ControllerTimeStatistics_;

    void IncreaseJobResourceUsage(TJobProperties& properties, const TJobResources& resourcesDelta);
};

DEFINE_REFCOUNTED_TYPE(TOperationElementSharedState)

class TOperationElement
    : public TSchedulerElement
    , public TOperationElementFixedState
{
public:
    TOperationElement(
        TFairShareStrategyConfigPtr strategyConfig,
        TStrategyOperationSpecPtr spec,
        TOperationRuntimeParamsPtr runtimeParams,
        ISchedulerStrategyHost* host,
        TOperationPtr operation);
    TOperationElement(
        const TOperationElement& other,
        TCompositeSchedulerElement* clonedParent);

    virtual double GetFairShareStarvationTolerance() const override;
    virtual TDuration GetMinSharePreemptionTimeout() const override;
    virtual TDuration GetFairSharePreemptionTimeout() const override;

    virtual void UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList) override;
    virtual void UpdateTopDown(TDynamicAttributesList& dynamicAttributesList) override;

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const override;

    virtual void UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList) override;

    virtual void PrescheduleJob(TFairShareContext& context, bool starvingOnly, bool aggressiveStarvationEnabled) override;
    virtual bool ScheduleJob(TFairShareContext& context) override;

    virtual Stroka GetId() const override;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    virtual double GetWeight() const override;
    virtual double GetMinShareRatio() const override;
    virtual TJobResources GetMinShareResources() const override;
    virtual double GetMaxShareRatio() const override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual ESchedulableStatus GetStatus() const override;

    virtual void SetStarving(bool starving) override;
    virtual void CheckForStarvation(TInstant now) override;
    bool HasStarvingParent() const;

    virtual void IncreaseResourceUsage(const TJobResources& delta) override;

    void IncreaseJobResourceUsage(const TJobId& jobId, const TJobResources& resourcesDelta);

    bool IsJobExisting(const TJobId& jobId) const;

    bool IsJobPreemptable(const TJobId& jobId, bool aggressivePreemptionEnabled) const;

    int GetRunningJobCount() const;
    int GetPreemptableJobCount() const;
    int GetAggressivelyPreemptableJobCount() const;

    void OnJobStarted(const TJobId& jobId, const TJobResources& resourceUsage);
    void OnJobFinished(const TJobId& jobId);

    virtual void BuildOperationToElementMapping(TOperationElementByIdMap* operationElementByIdMap) override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;

    TJobResources Finalize();

    DEFINE_BYVAL_RW_PROPERTY(TOperationRuntimeParamsPtr, RuntimeParams);

    DEFINE_BYVAL_RO_PROPERTY(TStrategyOperationSpecPtr, Spec);

    TSchedulingTagFilter SchedulingTagFilter_;

private:
    TOperationElementSharedStatePtr SharedState_;

    bool IsBlocked(NProfiling::TCpuInstant now) const;

    TJobResources GetHierarchicalResourceLimits(const TFairShareContext& context) const;

    TScheduleJobResultPtr DoScheduleJob(TFairShareContext& context);

    TJobResources ComputeResourceDemand() const;
    TJobResources ComputeResourceLimits() const;
    TJobResources ComputeMaxPossibleResourceUsage() const;
    int ComputePendingJobCount() const;

};

DEFINE_REFCOUNTED_TYPE(TOperationElement)

////////////////////////////////////////////////////////////////////////////////

class TRootElementFixedState
{
public:
    DEFINE_BYVAL_RO_PROPERTY(int, TreeSize);
};

class TRootElement
    : public TCompositeSchedulerElement
    , public TRootElementFixedState
{
public:
    TRootElement(
        ISchedulerStrategyHost* host,
        TFairShareStrategyConfigPtr strategyConfig);
    TRootElement(const TRootElement& other);

    virtual void Update(TDynamicAttributesList& dynamicAttributesList) override;

    virtual bool IsRoot() const override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual Stroka GetId() const override;

    virtual double GetWeight() const override;
    virtual double GetMinShareRatio() const override;
    virtual TJobResources GetMinShareResources() const override;
    virtual double GetMaxShareRatio() const override;

    virtual double GetFairShareStarvationTolerance() const override;
    virtual TDuration GetMinSharePreemptionTimeout() const override;
    virtual TDuration GetFairSharePreemptionTimeout() const override;

    virtual void CheckForStarvation(TInstant now) override;

    virtual int GetMaxRunningOperationCount() const override;
    virtual int GetMaxOperationCount() const override;

    virtual bool AreImmediateOperationsFobidden() const override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;
    TRootElementPtr Clone();

};

DEFINE_REFCOUNTED_TYPE(TRootElement)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

#define FAIR_SHARE_TREE_INL_H_
#include "fair_share_tree-inl.h"
#undef FAIR_SHARE_TREE_INL_H_
