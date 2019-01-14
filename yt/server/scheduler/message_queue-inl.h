#pragma once

#ifndef MESSAGE_QUEUE_INL_H_
#error "Direct inclusion of this file is not allowed, include message_queue.h"
// For the sake of sane code completion.
#include "message_queue.h"
#endif

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

template <class TItem>
TMessageQueueOutbox<TItem>::TMessageQueueOutbox(const NLogging::TLogger& logger)
    : Logger(logger)
{ }

template <class TItem>
void TMessageQueueOutbox<TItem>::Enqueue(TItem&& item)
{
    VERIFY_THREAD_AFFINITY_ANY();

    Stack_.Enqueue(std::move(item));
}

template <class TItem>
void TMessageQueueOutbox<TItem>::Enqueue(std::vector<TItem>&& items)
{
    VERIFY_THREAD_AFFINITY_ANY();

    Stack_.Enqueue(std::move(items));
}

template <class TItem>
template <class TProtoMessage, class TBuilder>
void TMessageQueueOutbox<TItem>::BuildOutcoming(TProtoMessage* message, TBuilder protoItemBuilder)
{
    VERIFY_THREAD_AFFINITY(Consumer);

    Stack_.DequeueAll(true, [&] (TEntry& entry) {
        if (auto* item = std::get_if<TItem>(&entry)) {
            Queue_.emplace(std::move(*item));
            ++NextItemId_;
        } else if (auto* items = std::get_if<std::vector<TItem>>(&entry)) {
            for (auto&& item : *items) {
                Queue_.emplace(std::move(item));
                ++NextItemId_;
            }
        } else {
            Y_UNREACHABLE();
        }
    });

    auto firstItemId = FirstItemId_;
    auto lastItemId = FirstItemId_ + Queue_.size() - 1;
    message->set_first_item_id(firstItemId);
    if (Queue_.empty()) {
        return;
    }
    for (auto it = Queue_.begin(); it != Queue_.end(); Queue_.move_forward(it)) {
        protoItemBuilder(message->add_items(), *it);
    }
    YT_LOG_DEBUG("Sending outbox items (ItemIds: %v-%v)",
        firstItemId,
        lastItemId);
}

template <class TItem>
template <class TProtoMessage>
void TMessageQueueOutbox<TItem>::HandleStatus(const TProtoMessage& message)
{
    VERIFY_THREAD_AFFINITY(Consumer);

    auto nextExpectedItemId = message.next_expected_item_id();
    YCHECK(nextExpectedItemId <= NextItemId_);
    if (nextExpectedItemId == FirstItemId_) {
        return;
    }
    if (nextExpectedItemId < FirstItemId_) {
        YT_LOG_DEBUG("Stale outbox items confirmed (NextExpectedItemId: %v, FirstItemId: %v)",
            nextExpectedItemId,
            FirstItemId_);
        return;
    }
    auto firstConfirmedItemId = FirstItemId_;
    auto lastConfirmedItemId = FirstItemId_;
    while (FirstItemId_ < nextExpectedItemId) {
        Queue_.pop();
        lastConfirmedItemId = FirstItemId_++;
    }
    YT_LOG_DEBUG("Outbox items confirmed (ItemIds: %v-%v)",
        firstConfirmedItemId,
        lastConfirmedItemId);
}

////////////////////////////////////////////////////////////////////////////////

inline TMessageQueueInbox::TMessageQueueInbox(const NLogging::TLogger& logger)
    : Logger(logger)
{ }

template <class TProtoRequest>
void TMessageQueueInbox::ReportStatus(TProtoRequest* request)
{
    VERIFY_THREAD_AFFINITY(Consumer);

    request->set_next_expected_item_id(NextExpectedItemId_);

    YT_LOG_DEBUG("Inbox status reported (NextExpectedItemId: %v)",
        NextExpectedItemId_);
}

template <class TProtoMessage, class TConsumer>
void TMessageQueueInbox::HandleIncoming(TProtoMessage* message, TConsumer protoItemConsumer)
{
    VERIFY_THREAD_AFFINITY(Consumer);

    if (message->items_size() == 0) {
        return;
    }

    auto firstConsumedItemId = -1;
    auto lastConsumedItemId = -1;
    auto itemId = message->first_item_id();
    for (auto& protoItem : *message->mutable_items()) {
        if (itemId == NextExpectedItemId_) {
            protoItemConsumer(&protoItem);
            if (firstConsumedItemId < 0) {
                firstConsumedItemId = itemId;
            }
            lastConsumedItemId = itemId;
            ++NextExpectedItemId_;
        }
        ++itemId;
    }

    if (firstConsumedItemId >= 0) {
        YT_LOG_DEBUG("Inbox items received and consumed (ReceivedIds: %v-%v, ConsumedIds: %v-%v)",
            message->first_item_id(),
            message->first_item_id() + message->items_size() - 1,
            firstConsumedItemId,
            lastConsumedItemId);
    } else {
        YT_LOG_DEBUG("Inbox items received but none consumed (ReceivedIds: %v-%v)",
            message->first_item_id(),
            message->first_item_id() + message->items_size() - 1);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
