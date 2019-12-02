#pragma once

#include "fair_share_strategy_operation_controller.h"
#include "job.h"
#include "private.h"
#include "resource_tree_element.h"
#include "scheduler_strategy.h"
#include "scheduling_context.h"
#include "fair_share_strategy_operation_controller.h"
#include "packing.h"
#include "historic_usage_aggregator.h"

#include <yt/server/lib/scheduler/config.h>
#include <yt/server/lib/scheduler/scheduling_tag.h>
#include <yt/server/lib/scheduler/job_metrics.h>

#include <yt/ytlib/scheduler/job_resources.h>

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT::NScheduler {

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
    int FifoIndex = -1;

    double AdjustedFairShareStarvationTolerance = 1.0;
    TDuration AdjustedMinSharePreemptionTimeout;
    TDuration AdjustedFairSharePreemptionTimeout;
};

//! Attributes that persistent between fair share updates.
struct TPersistentAttributes
{
    bool Starving = false;
    TInstant LastNonStarvingTime;
    std::optional<TInstant> BelowFairShareSince;
    THistoricUsageAggregator HistoricUsageAggregator;
};

struct TDynamicAttributes
{
    double SatisfactionRatio = 0.0;
    bool Active = false;
    TSchedulerElement* BestLeafDescendant = nullptr;
    TJobResources ResourceUsageDiscount;
};

typedef std::vector<TDynamicAttributes> TDynamicAttributesList;


////////////////////////////////////////////////////////////////////////////////

//! This interface must be thread-safe.
struct IFairShareTreeHost
{
    virtual TResourceTree* GetResourceTree() = 0;

    virtual NProfiling::TAggregateGauge& GetProfilingCounter(const TString& name) = 0;
};
////////////////////////////////////////////////////////////////////////////////

struct TUpdateFairShareContext
{
    std::vector<TError> Errors;
    THashMap<TString, int> ElementIndexes;
    TInstant Now;
    TEnumIndexedVector<EUnschedulableReason, int> UnschedulableReasons;
};

////////////////////////////////////////////////////////////////////////////////

struct TScheduleJobsProfilingCounters
{
    TScheduleJobsProfilingCounters(const TString& prefix, const NProfiling::TTagIdList& treeIdProfilingTags);

    NProfiling::TAggregateGauge PrescheduleJobTime;
    NProfiling::TAggregateGauge TotalControllerScheduleJobTime;
    NProfiling::TAggregateGauge ExecControllerScheduleJobTime;
    NProfiling::TAggregateGauge StrategyScheduleJobTime;
    NProfiling::TAggregateGauge PackingRecordHeartbeatTime;
    NProfiling::TAggregateGauge PackingCheckTime;
    NProfiling::TMonotonicCounter ScheduleJobCount;
    NProfiling::TMonotonicCounter ScheduleJobFailureCount;
    TEnumIndexedVector<NControllerAgent::EScheduleJobFailReason, NProfiling::TMonotonicCounter> ControllerScheduleJobFail;
};

////////////////////////////////////////////////////////////////////////////////

struct TFairShareSchedulingStage
{
    TFairShareSchedulingStage(const TString& loggingName, TScheduleJobsProfilingCounters profilingCounters);

    const TString LoggingName;
    TScheduleJobsProfilingCounters ProfilingCounters;
};

////////////////////////////////////////////////////////////////////////////////

class TFairShareContext
{
public:
    TFairShareContext(
        const ISchedulingContextPtr& schedulingContext,
        bool enableSchedulingInfoLogging,
        const NLogging::TLogger& logger);

    void Initialize(int treeSize, const std::vector<TSchedulingTagFilter>& registeredSchedulingTagFilters);

    TDynamicAttributes& DynamicAttributesFor(const TSchedulerElement* element);
    const TDynamicAttributes& DynamicAttributesFor(const TSchedulerElement* element) const;

    void StartStage(TFairShareSchedulingStage* schedulingStage);

    void ProfileStageTimingsAndLogStatistics();

    void FinishStage();

    bool Initialized = false;

    std::vector<bool> CanSchedule;
    TDynamicAttributesList DynamicAttributesList;

    const ISchedulingContextPtr SchedulingContext;
    const bool EnableSchedulingInfoLogging;

    // Used to avoid unnecessary calculation of HasAggressivelyStarvingElements.
    bool PrescheduleCalled = false;

    TFairShareSchedulingStatistics SchedulingStatistics;

    std::vector<TOperationElementPtr> BadPackingOperations;

    struct TStageState
    {
        explicit TStageState(TFairShareSchedulingStage* schedulingStage);

        TFairShareSchedulingStage* const SchedulingStage;

        TDuration TotalDuration;
        TDuration PrescheduleDuration;
        TDuration TotalScheduleJobDuration;
        TDuration ExecScheduleJobDuration;
        TDuration PackingRecordHeartbeatDuration;
        TDuration PackingCheckDuration;
        TEnumIndexedVector<NControllerAgent::EScheduleJobFailReason, int> FailedScheduleJob;

        int ActiveOperationCount = 0;
        int ActiveTreeSize = 0;
        int ScheduleJobAttempts = 0;
        int ScheduleJobFailureCount = 0;
        TEnumIndexedVector<EDeactivationReason, int> DeactivationReasons;
    };

    std::optional<TStageState> StageState;

private:
    const NLogging::TLogger Logger;

    void ProfileStageTimings();

    void LogStageStatistics();
};

////////////////////////////////////////////////////////////////////////////////

struct TSchedulerElementStateSnapshot
{
    TJobResources ResourceDemand;
    TJobResources MinShareResources;
};

////////////////////////////////////////////////////////////////////////////////

const int UnassignedTreeIndex = -1;
const int EmptySchedulingTagFilterIndex = -1;

class TSchedulerElementFixedState
{
public:
    DEFINE_BYREF_RO_PROPERTY(TJobResources, ResourceDemand);
    DEFINE_BYREF_RO_PROPERTY(TJobResources, ResourceLimits, TJobResources::Infinite());
    DEFINE_BYREF_RO_PROPERTY(TJobResources, MaxPossibleResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(TSchedulableAttributes, Attributes);
    DEFINE_BYREF_RW_PROPERTY(TPersistentAttributes, PersistentAttributes);
    DEFINE_BYVAL_RW_PROPERTY(int, SchedulingTagFilterIndex, EmptySchedulingTagFilterIndex);

protected:
    TSchedulerElementFixedState(
        ISchedulerStrategyHost* host,
        IFairShareTreeHost* treeHost,
        const TFairShareStrategyTreeConfigPtr& treeConfig,
        const TString& treeId);

    ISchedulerStrategyHost* const Host_;
    IFairShareTreeHost* const TreeHost_;

    TFairShareStrategyTreeConfigPtr TreeConfig_;

    TCompositeSchedulerElement* Parent_ = nullptr;

    TJobResources TotalResourceLimits_;

    int PendingJobCount_ = 0;
    TInstant StartTime_;

    int TreeIndex_ = UnassignedTreeIndex;

    // TODO(ignat): it is not used anymore, consider deleting.
    bool Cloned_ = false;
    bool Mutable_ = true;

    const TString TreeId_;
};

////////////////////////////////////////////////////////////////////////////////
struct TFairShareScheduleJobResult
{
    TFairShareScheduleJobResult(bool finished, bool scheduled)
        : Finished(finished)
        , Scheduled(scheduled)
    { }

    bool Finished;
    bool Scheduled;
};

class TSchedulerElement
    : public TSchedulerElementFixedState
    , public TIntrinsicRefCounted
{
public:
    //! Enumerates elements of the tree using inorder traversal. Returns first unused index.
    virtual int EnumerateElements(int startIndex, TUpdateFairShareContext* context);

    int GetTreeIndex() const;
    void SetTreeIndex(int treeIndex);

    virtual void DisableNonAliveElements() = 0;

    virtual void MarkUnmutable();

    virtual void UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config);

    //! Updates attributes that need to be computed from leafs up to root.
    //! For example: |parent->ResourceDemand = Sum(child->ResourceDemand)|.
    virtual void UpdateBottomUp(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);

    //! Updates attributes that are propagated from root down to leafs.
    //! For example: |child->FairShareRatio = fraction(parent->FairShareRatio)|.
    virtual void UpdateTopDown(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const = 0;

    virtual void UpdateDynamicAttributes(TDynamicAttributesList* dynamicAttributesList);

    virtual void PrescheduleJob(TFairShareContext* context, bool starvingOnly, bool aggressiveStarvationEnabled);
    virtual TFairShareScheduleJobResult ScheduleJob(TFairShareContext* context, bool ignorePacking) = 0;

    virtual bool HasAggressivelyStarvingElements(TFairShareContext* context, bool aggressiveStarvationEnabled) const = 0;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const;

    virtual bool IsRoot() const;
    virtual bool IsOperation() const;

    virtual TString GetLoggingString(const TDynamicAttributes& dynamicAttributes) const;

    bool IsActive(const TDynamicAttributesList& dynamicAttributesList) const;

    virtual bool IsSchedulable() const = 0;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const = 0;

    bool IsAlive() const;
    void SetAlive(bool alive);

    double GetFairShareRatio() const;
    void SetFairShareRatio(double fairShareRatio);

    virtual TString GetId() const = 0;

    virtual std::optional<double> GetSpecifiedWeight() const = 0;
    virtual double GetWeight() const;

    virtual double GetMinShareRatio() const = 0;
    virtual TJobResources GetMinShareResources() const = 0;

    virtual double GetMaxShareRatio() const = 0;

    virtual double GetFairShareStarvationTolerance() const = 0;
    virtual TDuration GetMinSharePreemptionTimeout() const = 0;
    virtual TDuration GetFairSharePreemptionTimeout() const = 0;

    TCompositeSchedulerElement* GetMutableParent();
    const TCompositeSchedulerElement* GetParent() const;

    TInstant GetStartTime() const;
    int GetPendingJobCount() const;

    virtual ESchedulableStatus GetStatus() const;

    bool GetStarving() const;
    virtual void SetStarving(bool starving);
    virtual void CheckForStarvation(TInstant now) = 0;

    TJobResources GetLocalResourceUsage() const;
    TJobMetrics GetJobMetrics() const;
    double GetResourceUsageRatio() const;
    double GetResourceUsageRatioWithPrecommit() const;

    virtual TString GetTreeId() const;

    void IncreaseHierarchicalResourceUsage(const TJobResources& delta);

    virtual void BuildElementMapping(
        TRawOperationElementMap* operationMap,
        TRawPoolMap* poolMap,
        TDisabledOperationsSet* disabledOperations) = 0;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) = 0;

    double ComputeLocalSatisfactionRatio() const;

    const NLogging::TLogger& GetLogger() const;

private:
    TResourceTreeElementPtr ResourceTreeElement_;

protected:
    TSchedulerElement(
        ISchedulerStrategyHost* host,
        IFairShareTreeHost* treeHost,
        const TFairShareStrategyTreeConfigPtr& treeConfig,
        const TString& treeId,
        const NLogging::TLogger& logger);
    TSchedulerElement(
        const TSchedulerElement& other,
        TCompositeSchedulerElement* clonedParent);

    ISchedulerStrategyHost* GetHost() const;

    IFairShareTreeHost* GetTreeHost() const;

    ESchedulableStatus GetStatus(double defaultTolerance) const;

    void CheckForStarvationImpl(
        TDuration minSharePreemptionTimeout,
        TDuration fairSharePreemptionTimeout,
        TInstant now);

    void SetOperationAlert(
        TOperationId operationId,
        EOperationAlertType alertType,
        const TError& alert,
        std::optional<TDuration> timeout);

    TString GetLoggingAttributesString(const TDynamicAttributes& dynamicAttributes) const;

    TJobResources GetLocalAvailableResourceDemand(const TFairShareContext& context) const;
    TJobResources GetLocalAvailableResourceLimits(const TFairShareContext& context) const;

    bool CheckDemand(const TJobResources& delta, const TFairShareContext& context);

    TJobResources ComputeResourceLimitsBase(const TResourceLimitsConfigPtr& resourceLimitsConfig) const;

protected:
    const NLogging::TLogger Logger;

private:
    void UpdateAttributes();
    double ComputeResourceUsageRatio(const TJobResources& jobResources) const;

    friend class TCompositeSchedulerElement;
    friend class TOperationElement;
    friend class TPool;
};

DEFINE_REFCOUNTED_TYPE(TSchedulerElement)

////////////////////////////////////////////////////////////////////////////////

class TCompositeSchedulerElementFixedState
{
public:
    DEFINE_BYREF_RW_PROPERTY(int, RunningOperationCount);
    DEFINE_BYREF_RW_PROPERTY(int, OperationCount);
    DEFINE_BYREF_RW_PROPERTY(std::list<TOperationId>, WaitingOperationIds);

    DEFINE_BYREF_RO_PROPERTY(double, AdjustedFairShareStarvationToleranceLimit);
    DEFINE_BYREF_RO_PROPERTY(TDuration, AdjustedMinSharePreemptionTimeoutLimit);
    DEFINE_BYREF_RO_PROPERTY(TDuration, AdjustedFairSharePreemptionTimeoutLimit);

protected:
    ESchedulingMode Mode_ = ESchedulingMode::Fifo;
    std::vector<EFifoSortParameter> FifoSortParameters_;
};

////////////////////////////////////////////////////////////////////////////////

class TCompositeSchedulerElement
    : public TSchedulerElement
    , public TCompositeSchedulerElementFixedState
{
public:
    TCompositeSchedulerElement(
        ISchedulerStrategyHost* host,
        IFairShareTreeHost* treeHost,
        TFairShareStrategyTreeConfigPtr treeConfig,
        NProfiling::TTagId profilingTag,
        const TString& treeId,
        const NLogging::TLogger& logger);
    TCompositeSchedulerElement(
        const TCompositeSchedulerElement& other,
        TCompositeSchedulerElement* clonedParent);

    virtual int EnumerateElements(int startIndex, TUpdateFairShareContext* context) override;

    virtual void DisableNonAliveElements() override;

    virtual void MarkUnmutable() override;

    virtual void UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config) override;

    virtual void UpdateBottomUp(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context) override;
    virtual void UpdateTopDown(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context) override;

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const override;

    virtual double GetFairShareStarvationToleranceLimit() const;
    virtual TDuration GetMinSharePreemptionTimeoutLimit() const;
    virtual TDuration GetFairSharePreemptionTimeoutLimit() const;

    void UpdatePreemptionSettingsLimits();
    void UpdateChildPreemptionSettings(const TSchedulerElementPtr& child);

    virtual void UpdateDynamicAttributes(TDynamicAttributesList* dynamicAttributesList) override;

    virtual void PrescheduleJob(TFairShareContext* context, bool starvingOnly, bool aggressiveStarvationEnabled) override;
    virtual TFairShareScheduleJobResult ScheduleJob(TFairShareContext* context, bool ignorePacking) override;

    virtual bool IsSchedulable() const override;

    virtual bool HasAggressivelyStarvingElements(TFairShareContext* context, bool aggressiveStarvationEnabled) const override;

    virtual bool IsExplicit() const;
    virtual bool IsAggressiveStarvationEnabled() const;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    void AddChild(TSchedulerElement* child, bool enabled = true);
    void EnableChild(const TSchedulerElementPtr& child);
    void DisableChild(const TSchedulerElementPtr& child);
    void RemoveChild(TSchedulerElement* child);
    bool IsEnabledChild(TSchedulerElement* child);

    bool IsEmpty() const;

    ESchedulingMode GetMode() const;
    void SetMode(ESchedulingMode);

    NProfiling::TTagId GetProfilingTag() const;

    virtual TJobResources GetSpecifiedResourceLimits() const = 0;
    virtual TJobResources ComputeResourceLimits() const = 0;

    virtual int GetMaxOperationCount() const = 0;
    virtual int GetMaxRunningOperationCount() const = 0;
    int GetAvailableRunningOperationCount() const;

    virtual std::vector<EFifoSortParameter> GetFifoSortParameters() const = 0;
    virtual bool AreImmediateOperationsForbidden() const = 0;

    virtual void BuildElementMapping(TRawOperationElementMap* operationMap, TRawPoolMap* poolMap, TDisabledOperationsSet* disabledOperations) override;

    void IncreaseOperationCount(int delta);
    void IncreaseRunningOperationCount(int delta);

    virtual THashSet<TString> GetAllowedProfilingTags() const = 0;

    virtual bool IsInferringChildrenWeightsFromHistoricUsageEnabled() const = 0;
    virtual THistoricUsageAggregationParameters GetHistoricUsageAggregationParameters() const = 0;

protected:
    const NProfiling::TTagId ProfilingTag_;

    using TChildMap = THashMap<TSchedulerElementPtr, int>;
    using TChildList = std::vector<TSchedulerElementPtr>;

    TChildMap EnabledChildToIndex_;
    TChildList EnabledChildren_;

    TChildMap DisabledChildToIndex_;
    TChildList DisabledChildren_;

    TChildList SchedulableChildren_;

    template <class TGetter, class TSetter>
    void ComputeByFitting(const TGetter& getter, const TSetter& setter, double sum);

    void UpdateFifo(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);
    void UpdateFairShare(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);

    TSchedulerElement* GetBestActiveChild(const TDynamicAttributesList& dynamicAttributesList) const;
    TSchedulerElement* GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const;
    TSchedulerElement* GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const;

    static void AddChild(TChildMap* map, TChildList* list, const TSchedulerElementPtr& child);
    static void RemoveChild(TChildMap* map, TChildList* list, const TSchedulerElementPtr& child);
    static bool ContainsChild(const TChildMap& map, const TSchedulerElementPtr& child);

private:
    bool HasHigherPriorityInFifoMode(const TSchedulerElement* lhs, const TSchedulerElement* rhs) const;
};

DEFINE_REFCOUNTED_TYPE(TCompositeSchedulerElement)

////////////////////////////////////////////////////////////////////////////////

class TPoolFixedState
{
protected:
    explicit TPoolFixedState(const TString& id);

    const TString Id_;
    bool DefaultConfigured_ = true;
    bool EphemeralInDefaultParentPool_ = false;
    std::optional<TString> UserName_;
};

////////////////////////////////////////////////////////////////////////////////

class TPool
    : public TCompositeSchedulerElement
    , public TPoolFixedState
{
public:
    TPool(
        ISchedulerStrategyHost* host,
        IFairShareTreeHost* treeHost,
        const TString& id,
        TPoolConfigPtr config,
        bool defaultConfigured,
        TFairShareStrategyTreeConfigPtr treeConfig,
        NProfiling::TTagId profilingTag,
        const TString& treeId,
        const NLogging::TLogger& logger);
    TPool(
        const TPool& other,
        TCompositeSchedulerElement* clonedParent);

    bool IsDefaultConfigured() const;
    bool IsEphemeralInDefaultParentPool() const;

    void SetUserName(const std::optional<TString>& userName);
    const std::optional<TString>& GetUserName() const;

    TPoolConfigPtr GetConfig();
    void SetConfig(TPoolConfigPtr config);
    void SetDefaultConfig();
    void SetEphemeralInDefaultParentPool();

    virtual bool IsExplicit() const override;
    virtual bool IsAggressiveStarvationEnabled() const override;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    virtual TString GetId() const override;

    virtual std::optional<double> GetSpecifiedWeight() const override;
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

    virtual TJobResources GetSpecifiedResourceLimits() const override;
    virtual TJobResources ComputeResourceLimits() const override;

    virtual void SetStarving(bool starving) override;
    virtual void CheckForStarvation(TInstant now) override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual void UpdateBottomUp(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context) override;

    virtual int GetMaxRunningOperationCount() const override;
    virtual int GetMaxOperationCount() const override;

    virtual std::vector<EFifoSortParameter> GetFifoSortParameters() const override;
    virtual bool AreImmediateOperationsForbidden() const override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;

    void AttachParent(TCompositeSchedulerElement* newParent);
    void ChangeParent(TCompositeSchedulerElement* newParent);
    void DetachParent();

    virtual THashSet<TString> GetAllowedProfilingTags() const override;

    virtual bool IsInferringChildrenWeightsFromHistoricUsageEnabled() const override;
    virtual THistoricUsageAggregationParameters GetHistoricUsageAggregationParameters() const override;

    virtual void BuildElementMapping(TRawOperationElementMap* operationMap, TRawPoolMap* poolMap, TDisabledOperationsSet* disabledOperations) override;
private:
    TPoolConfigPtr Config_;
    TSchedulingTagFilter SchedulingTagFilter_;

    void DoSetConfig(TPoolConfigPtr newConfig);
};

DEFINE_REFCOUNTED_TYPE(TPool)

////////////////////////////////////////////////////////////////////////////////

class TOperationElementFixedState
{
protected:
    TOperationElementFixedState(
        IOperationStrategyHost* operation,
        TFairShareStrategyOperationControllerConfigPtr controllerConfig);

    const TOperationId OperationId_;
    std::optional<EUnschedulableReason> UnschedulableReason_;
    std::optional<int> SlotIndex_;
    TString UserName_;
    IOperationStrategyHost* const Operation_;
    TFairShareStrategyOperationControllerConfigPtr ControllerConfig_;
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EOperationPreemptionStatus,
    (Allowed)
    (ForbiddenSinceStarvingParent)
    (ForbiddenSinceUnsatisfiedParentOrSelf)
    (ForbiddenSinceLowJobCount)
);

using TPreemptionStatusStatisticsVector = TEnumIndexedVector<EOperationPreemptionStatus, int>;

class TOperationElementSharedState
    : public TIntrinsicRefCounted
{
public:
    TOperationElementSharedState(
        int updatePreemptableJobsListLoggingPeriod,
        const NLogging::TLogger& logger);

    TJobResources IncreaseJobResourceUsage(
        TJobId jobId,
        const TJobResources& resourcesDelta);

    void UpdatePreemptableJobsList(
        double fairShareRatio,
        const TJobResources& totalResourceLimits,
        double preemptionSatisfactionThreshold,
        double aggressivePreemptionSatisfactionThreshold,
        int* moveCount);

    void SetPreemptable(bool value);

    bool IsJobKnown(TJobId jobId) const;

    bool IsJobPreemptable(TJobId jobId, bool aggressivePreemptionEnabled) const;

    int GetRunningJobCount() const;
    int GetPreemptableJobCount() const;
    int GetAggressivelyPreemptableJobCount() const;

    std::optional<TJobResources> AddJob(TJobId jobId, const TJobResources& resourceUsage, bool force);
    std::optional<TJobResources> RemoveJob(TJobId jobId);

    void UpdatePreemptionStatusStatistics(EOperationPreemptionStatus status);
    TPreemptionStatusStatisticsVector GetPreemptionStatusStatistics() const;

    void OnOperationDeactivated(const TFairShareContext& context, EDeactivationReason reason);
    TEnumIndexedVector<EDeactivationReason, int> GetDeactivationReasons() const;
    void ResetDeactivationReasonsFromLastNonStarvingTime();
    TEnumIndexedVector<EDeactivationReason, int> GetDeactivationReasonsFromLastNonStarvingTime() const;

    TInstant GetLastScheduleJobSuccessTime() const;

    TJobResources Disable();
    void Enable();
    bool Enabled();

    void RecordHeartbeat(
        const TPackingHeartbeatSnapshot& heartbeatSnapshot,
        const TFairShareStrategyPackingConfigPtr& config);
    bool CheckPacking(
        const TOperationElement* operationElement,
        const TPackingHeartbeatSnapshot& heartbeatSnapshot,
        const TJobResourcesWithQuota& jobResources,
        const TJobResources& totalResourceLimits,
        const TFairShareStrategyPackingConfigPtr& config);

private:
    using TJobIdList = std::list<TJobId>;

    TJobIdList NonpreemptableJobs_;
    TJobIdList AggressivelyPreemptableJobs_;
    TJobIdList PreemptableJobs_;

    std::atomic<bool> Preemptable_ = {true};

    std::atomic<int> RunningJobCount_ = {0};
    TJobResources NonpreemptableResourceUsage_;
    TJobResources AggressivelyPreemptableResourceUsage_;

    std::atomic<int> UpdatePreemptableJobsListCount_ = {0};
    const int UpdatePreemptableJobsListLoggingPeriod_;

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

        //! Determines whether job belongs to list of preemptable or aggressively preemptable jobs of operation.
        bool Preemptable;

        //! Determines whether job belongs to list of preemptable (but not aggressively preemptable) jobs of operation.
        bool AggressivelyPreemptable;

        //! Iterator in the per-operation list pointing to this particular job.
        TJobIdList::iterator JobIdListIterator;

        TJobResources ResourceUsage;
    };

    NConcurrency::TReaderWriterSpinLock JobPropertiesMapLock_;
    THashMap<TJobId, TJobProperties> JobPropertiesMap_;
    TInstant LastScheduleJobSuccessTime_;

    TSpinLock PreemptionStatusStatisticsLock_;
    TPreemptionStatusStatisticsVector PreemptionStatusStatistics_;

    const NLogging::TLogger Logger;

    struct TStateShard
    {
        TEnumIndexedVector<EDeactivationReason, std::atomic<int>> DeactivationReasons;
        TEnumIndexedVector<EDeactivationReason, std::atomic<int>> DeactivationReasonsFromLastNonStarvingTime;
        char Padding[64];
    };
    std::array<TStateShard, MaxNodeShardCount> StateShards_;

    bool Enabled_ = false;

    TPackingStatistics HeartbeatStatistics_;

    void IncreaseJobResourceUsage(TJobProperties* properties, const TJobResources& resourcesDelta);

    TJobProperties* GetJobProperties(TJobId jobId);
    const TJobProperties* GetJobProperties(TJobId jobId) const;
};

DEFINE_REFCOUNTED_TYPE(TOperationElementSharedState)

class TOperationElement
    : public TSchedulerElement
    , public TOperationElementFixedState
{
public:
    TOperationElement(
        TFairShareStrategyTreeConfigPtr treeConfig,
        TStrategyOperationSpecPtr spec,
        TOperationFairShareTreeRuntimeParametersPtr runtimeParameters,
        TFairShareStrategyOperationControllerPtr controller,
        TFairShareStrategyOperationControllerConfigPtr controllerConfig,
        ISchedulerStrategyHost* host,
        IFairShareTreeHost* treeHost,
        IOperationStrategyHost* operation,
        const TString& treeId,
        const NLogging::TLogger& logger);
    TOperationElement(
        const TOperationElement& other,
        TCompositeSchedulerElement* clonedParent);

    virtual void DisableNonAliveElements() override;

    virtual double GetFairShareStarvationTolerance() const override;
    virtual TDuration GetMinSharePreemptionTimeout() const override;
    virtual TDuration GetFairSharePreemptionTimeout() const override;

    virtual void UpdateBottomUp(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context) override;
    virtual void UpdateTopDown(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context) override;

    virtual bool IsOperation() const override;

    void UpdateControllerConfig(const TFairShareStrategyOperationControllerConfigPtr& config);

    virtual TJobResources ComputePossibleResourceUsage(TJobResources limit) const override;

    virtual void UpdateDynamicAttributes(TDynamicAttributesList* dynamicAttributesList) override;

    virtual void PrescheduleJob(TFairShareContext* context, bool starvingOnly, bool aggressiveStarvationEnabled) override;
    virtual TFairShareScheduleJobResult ScheduleJob(TFairShareContext* context, bool ignorePacking) override;

    virtual bool HasAggressivelyStarvingElements(TFairShareContext* context, bool aggressiveStarvationEnabled) const override;

    virtual TString GetLoggingString(const TDynamicAttributes& dynamicAttributes) const override;

    virtual TString GetId() const override;

    virtual bool IsSchedulable() const override;

    virtual bool IsAggressiveStarvationPreemptionAllowed() const override;

    virtual std::optional<double> GetSpecifiedWeight() const override;
    virtual double GetMinShareRatio() const override;
    virtual TJobResources GetMinShareResources() const override;
    virtual double GetMaxShareRatio() const override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual ESchedulableStatus GetStatus() const override;

    virtual void SetStarving(bool starving) override;
    virtual void CheckForStarvation(TInstant now) override;
    bool IsPreemptionAllowed(const TFairShareContext& context, const TFairShareStrategyTreeConfigPtr& config) const;

    void ApplyJobMetricsDelta(const TJobMetrics& delta);

    void IncreaseJobResourceUsage(TJobId jobId, const TJobResources& resourcesDelta);

    bool IsJobKnown(TJobId jobId) const;

    bool IsJobPreemptable(TJobId jobId, bool aggressivePreemptionEnabled) const;

    int GetRunningJobCount() const;
    int GetPreemptableJobCount() const;
    int GetAggressivelyPreemptableJobCount() const;

    TPreemptionStatusStatisticsVector GetPreemptionStatusStatistics() const;

    TInstant GetLastNonStarvingTime() const;
    TInstant GetLastScheduleJobSuccessTime() const;

    std::optional<int> GetMaybeSlotIndex() const;

    TString GetUserName() const;

    bool DetailedLogsEnabled() const;

    bool OnJobStarted(
        TJobId jobId,
        const TJobResources& resourceUsage,
        const TJobResources& precommittedResources,
        bool force = false);
    void OnJobFinished(TJobId jobId);

    virtual void BuildElementMapping(TRawOperationElementMap* operationMap, TRawPoolMap* poolMap, TDisabledOperationsSet* disabledOperations) override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;

    void OnOperationDeactivated(const TFairShareContext& context, EDeactivationReason reason);
    TEnumIndexedVector<EDeactivationReason, int> GetDeactivationReasons() const;
    TEnumIndexedVector<EDeactivationReason, int> GetDeactivationReasonsFromLastNonStarvingTime() const;

    std::optional<NProfiling::TTagId> GetCustomProfilingTag();

    void Disable();
    void Enable();

    bool TryIncreaseHierarchicalResourceUsagePrecommit(
        const TJobResources& delta,
        TJobResources* availableResourceLimitsOutput = nullptr);

    void AttachParent(TCompositeSchedulerElement* newParent, bool enabled);
    void ChangeParent(TCompositeSchedulerElement* newParent);
    void DetachParent();

    void MarkOperationRunningInPool();
    bool IsOperationRunningInPool();

    void UpdateAncestorsDynamicAttributes(TFairShareContext* context, bool activateAncestors = false);

    void MarkWaitingFor(TCompositeSchedulerElement* violatedPool);

    DEFINE_BYVAL_RW_PROPERTY(TOperationFairShareTreeRuntimeParametersPtr, RuntimeParameters);

    DEFINE_BYVAL_RO_PROPERTY(TStrategyOperationSpecPtr, Spec);

    DEFINE_BYREF_RW_PROPERTY(std::optional<TString>, WaitingForPool);

private:
    const TOperationElementSharedStatePtr OperationElementSharedState_;
    const TFairShareStrategyOperationControllerPtr Controller_;

    bool RunningInThisPoolTree_ = false;
    TSchedulingTagFilter SchedulingTagFilter_;

    bool HasJobsSatisfyingResourceLimits(const TFairShareContext& context) const;

    bool IsMaxConcurrentScheduleJobCallsViolated() const;
    bool HasRecentScheduleJobFailure(NProfiling::TCpuInstant now) const;
    std::optional<EDeactivationReason> CheckBlocked(NProfiling::TCpuInstant now) const;

    void RecordHeartbeat(const TPackingHeartbeatSnapshot& heartbeatSnapshot);
    bool CheckPacking(const TPackingHeartbeatSnapshot& heartbeatSnapshot) const;

    TJobResources GetHierarchicalAvailableResources(const TFairShareContext& context) const;

    std::optional<EDeactivationReason> TryStartScheduleJob(
        const TFairShareContext& context,
        TJobResources* precommittedResourcesOutput,
        TJobResources* availableResourcesOutput);

    void FinishScheduleJob(
        bool enableBackoff,
        NProfiling::TCpuInstant now);

    TControllerScheduleJobResultPtr DoScheduleJob(
        TFairShareContext* context,
        const TJobResources& availableResources,
        TJobResources* precommittedResources);

    TJobResources GetSpecifiedResourceLimits() const;
    TJobResources ComputeResourceLimits() const;
    TJobResources ComputeResourceDemand() const;
    TJobResources ComputeMaxPossibleResourceUsage() const;
    int ComputePendingJobCount() const;

    void UpdatePreemptableJobsList();

    TFairShareStrategyPackingConfigPtr GetPackingConfig() const;
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
        IFairShareTreeHost* treeHost,
        TFairShareStrategyTreeConfigPtr treeConfig,
        NProfiling::TTagId profilingTag,
        const TString& treeId,
        const NLogging::TLogger& logger);
    TRootElement(const TRootElement& other);

    //! Computes various lightweight attributes in the tree. Thread-unsafe.
    void PreUpdate(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);
    //! Computes min share ratio and fair share ratio in the tree. Thread-safe.
    void Update(TDynamicAttributesList* dynamicAttributesList, TUpdateFairShareContext* context);

    virtual void UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config) override;

    virtual bool IsRoot() const override;

    virtual const TSchedulingTagFilter& GetSchedulingTagFilter() const override;

    virtual TString GetId() const override;

    virtual std::optional<double> GetSpecifiedWeight() const override;
    virtual double GetMinShareRatio() const override;
    virtual TJobResources GetMinShareResources() const override;
    virtual double GetMaxShareRatio() const override;

    virtual double GetFairShareStarvationTolerance() const override;
    virtual TDuration GetMinSharePreemptionTimeout() const override;
    virtual TDuration GetFairSharePreemptionTimeout() const override;

    virtual TJobResources GetSpecifiedResourceLimits() const override;
    virtual TJobResources ComputeResourceLimits() const override;

    virtual bool IsAggressiveStarvationEnabled() const override;

    virtual void CheckForStarvation(TInstant now) override;

    virtual int GetMaxRunningOperationCount() const override;
    virtual int GetMaxOperationCount() const override;

    virtual std::vector<EFifoSortParameter> GetFifoSortParameters() const override;
    virtual bool AreImmediateOperationsForbidden() const override;

    virtual THashSet<TString> GetAllowedProfilingTags() const override;

    virtual bool IsInferringChildrenWeightsFromHistoricUsageEnabled() const override;
    virtual THistoricUsageAggregationParameters GetHistoricUsageAggregationParameters() const override;

    virtual TSchedulerElementPtr Clone(TCompositeSchedulerElement* clonedParent) override;
    TRootElementPtr Clone();
};

DEFINE_REFCOUNTED_TYPE(TRootElement)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

#define FAIR_SHARE_TREE_ELEMENT_INL_H_
#include "fair_share_tree_element-inl.h"
#undef FAIR_SHARE_TREE_ELEMENT_INL_H_
