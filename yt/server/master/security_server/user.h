#pragma once

#include "public.h"
#include "subject.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/object_server/object.h>

#include <yt/core/yson/consumer.h>

#include <yt/core/misc/property.h>

#include <yt/core/concurrency/public.h>

namespace NYT::NSecurityServer {

////////////////////////////////////////////////////////////////////////////////

struct TUserStatistics
{
    i64 RequestCount = 0;
    TDuration ReadRequestTime;
    TDuration WriteRequestTime;
    TInstant AccessTime;

    void Persist(NCellMaster::TPersistenceContext& context);
};

void ToProto(NProto::TUserStatistics* protoStatistics, const TUserStatistics& statistics);
void FromProto(TUserStatistics* statistics, const NProto::TUserStatistics& protoStatistics);

void Serialize(const TUserStatistics& statistics, NYson::IYsonConsumer* consumer);

TUserStatistics& operator += (TUserStatistics& lhs, const TUserStatistics& rhs);
TUserStatistics  operator +  (const TUserStatistics& lhs, const TUserStatistics& rhs);

////////////////////////////////////////////////////////////////////////////////

class TUser
    : public TSubject
{
public:
    // Limits and bans.
    DEFINE_BYVAL_RW_PROPERTY(bool, Banned, false);

    DEFINE_BYVAL_RW_PROPERTY(int, RequestQueueSizeLimit, 100);
    DEFINE_BYVAL_RW_PROPERTY(int, RequestQueueSize, 0);

    // Statistics
    using TMulticellStatistics = THashMap<NObjectClient::TCellTag, TUserStatistics>;
    DEFINE_BYREF_RW_PROPERTY(TMulticellStatistics, MulticellStatistics);
    DEFINE_BYVAL_RW_PROPERTY(TUserStatistics*, LocalStatisticsPtr);
    DEFINE_BYREF_RW_PROPERTY(TUserStatistics, ClusterStatistics);
    DEFINE_BYVAL_RW_PROPERTY(int, RequestStatisticsUpdateIndex, -1);

public:
    explicit TUser(TUserId id);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    TUserStatistics& CellStatistics(NObjectClient::TCellTag cellTag);
    TUserStatistics& LocalStatistics();

    void RecomputeClusterStatistics();

    const NConcurrency::IReconfigurableThroughputThrottlerPtr& GetRequestRateThrottler(EUserWorkloadType workloadType);
    void SetRequestRateThrottler(NConcurrency::IReconfigurableThroughputThrottlerPtr throttler, EUserWorkloadType workloadType);

    int GetRequestRateLimit(EUserWorkloadType workloadType);
    void SetRequestRateLimit(int limit, EUserWorkloadType workloadType);

private:
    NConcurrency::IReconfigurableThroughputThrottlerPtr ReadRequestRateThrottler_;
    NConcurrency::IReconfigurableThroughputThrottlerPtr WriteRequestRateThrottler_;

    int ReadRequestRateLimit_ = 100;
    int WriteRequestRateLimit_ = 100;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer
