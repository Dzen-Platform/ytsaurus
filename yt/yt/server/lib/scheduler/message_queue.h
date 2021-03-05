#pragma once

#include "public.h"

#include <yt/yt/core/misc/ring_queue.h>
#include <yt/yt/core/misc/lock_free.h>
#include <yt/yt/core/misc/variant.h>

#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/logging/log.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

using TMessageQueueItemId = i64;

template <class TItem>
class TMessageQueueOutbox
    : public TRefCounted
{
public:
    explicit TMessageQueueOutbox(const NLogging::TLogger& logger);

    /*
     * \note Thread affinity: any
     */
    void Enqueue(TItem&& item);
    /*
     * \note Thread affinity: any
     */
    void Enqueue(std::vector<TItem>&& items);

    /*
     * \note Thread affinity: single-threaded
     */
    template <class TProtoMessage, class TBuilder>
    void BuildOutcoming(TProtoMessage* message, TBuilder protoItemBuilder);

    /*
     * \note Thread affinity: single-threaded
     */
    template <class TProtoMessage>
    void HandleStatus(const TProtoMessage& message);

private:
    const NLogging::TLogger Logger;

    using TEntry = std::variant<TItem, std::vector<TItem>>;
    TMultipleProducerSingleConsumerLockFreeStack<TEntry> Stack_;

    TRingQueue<TItem> Queue_;
    TMessageQueueItemId FirstItemId_ = 0;
    TMessageQueueItemId NextItemId_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(Consumer);
};

////////////////////////////////////////////////////////////////////////////////

/*
 * \note Thread affinity: single-threaded
 */
class TMessageQueueInbox
{
public:
    explicit TMessageQueueInbox(const NLogging::TLogger& logger);

    template <class TProtoMessage>
    void ReportStatus(TProtoMessage* request);

    template <class TProtoMessage, class TConsumer>
    void HandleIncoming(TProtoMessage* message, TConsumer protoItemConsumer);

private:
    const NLogging::TLogger Logger;

    TMessageQueueItemId NextExpectedItemId_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(Consumer);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

#define MESSAGE_QUEUE_INL_H_
#include "message_queue-inl.h"
#undef MESSAGE_QUEUE_INL_H_
