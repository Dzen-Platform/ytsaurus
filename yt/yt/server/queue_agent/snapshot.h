#pragma once

#include "dynamic_state.h"
#include "performance_counters.h"

namespace NYT::NQueueAgent {

////////////////////////////////////////////////////////////////////////////////

//! Snapshot of a queue.
struct TQueueSnapshot
    : public TRefCounted
{
    TQueueTableRow Row;

    TError Error;

    EQueueFamily Family;
    int PartitionCount = 0;

    std::vector<TQueuePartitionSnapshotPtr> PartitionSnapshots;
    THashMap<NQueueClient::TCrossClusterReference, TConsumerSnapshotPtr> ConsumerSnapshots;

    //! Total write counters over all partitions.
    TPerformanceCounters WriteRate;

    bool HasTimestampColumn = false;
    bool HasCumulativeDataWeightColumn = false;
};

DEFINE_REFCOUNTED_TYPE(TQueueSnapshot);

////////////////////////////////////////////////////////////////////////////////

//! Snapshot of a partition within queue.
struct TQueuePartitionSnapshot
    : public TRefCounted
{
    TError Error;

    // Fields below are not set if error is set.
    i64 LowerRowIndex = -1;
    i64 UpperRowIndex = -1;
    i64 AvailableRowCount = -1;
    TInstant LastRowCommitTime;
    TDuration CommitIdleTime;

    i64 CumulativeDataWeight = 0;

    //! Write counters for the given partition.
    TPerformanceCounters WriteRate;

    //! Meta-information specific to given queue family.
    NYson::TYsonString Meta;
};

DEFINE_REFCOUNTED_TYPE(TQueuePartitionSnapshot);

////////////////////////////////////////////////////////////////////////////////

//! Snapshot of a consumer.
struct TConsumerSnapshot
    : public TRefCounted
{
    TConsumerTableRow Row;

    TError Error;

    NQueueClient::TCrossClusterReference TargetQueue;
    bool Vital = false;

    TString Owner;
    i64 PartitionCount = 0;

    std::vector<TConsumerPartitionSnapshotPtr> PartitionSnapshots;

    //! Total read counters over all partitions.
    TPerformanceCounters ReadRate;
};

DEFINE_REFCOUNTED_TYPE(TConsumerSnapshot);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EConsumerPartitionDisposition,
    //! Sentinel value.
    (None)
    //! At the end of the window, i.e. unread row count == 0.
    (UpToDate)
    //! Inside the window but not at the end, i.e. 0 < unread row count <= available row count.
    (PendingConsumption)
    //! Past the window, i.e. unread row count > available row count.
    (Expired)
    //! Ahead of the window, i.e. "unread row count < 0" (unread row count is capped)
    (Ahead)
)

//! Snapshot of a partition within consumer.
struct TConsumerPartitionSnapshot
    : public TRefCounted
{
    // The field below is effectively the error of the corresponding queue partition.

    TError Error;

    // Fields below are always set.
    i64 NextRowIndex = -1;
    TInstant LastConsumeTime;
    TDuration ConsumeIdleTime;

    // Fields below are not set if error is set (as they depend on the unavailable information on the queue partition).

    EConsumerPartitionDisposition Disposition = EConsumerPartitionDisposition::None;
    //! Offset of the next row with respect to the upper row index in the partition.
    //! May be negative if the consumer is ahead of the partition.
    i64 UnreadRowCount = -1;
    //! Amount of data unread by the consumer. Zero if the consumer is aheaed of the partition, expired or "almost expired".
    i64 UnreadDataWeight = -1;
    //! If #Disposition == PendingConsumption and commit timestamp is set up, the commit timestamp of the next row to be read by the consumer;
    //! std::nullopt otherwise.
    std::optional<TInstant> NextRowCommitTime;
    //! If #NextRowCommitTime is set, difference between Now() and *NextRowCommitTime; zero otherwise.
    TDuration ProcessingLag;

    i64 CumulativeDataWeight = 0;

    //! Read counters of the given consumer for the partition.
    TPerformanceCounters ReadRate;
};

DEFINE_REFCOUNTED_TYPE(TConsumerPartitionSnapshot);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
