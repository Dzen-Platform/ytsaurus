#pragma once

#include "public.h"
#include "helpers.h"

#include <yt/yt/server/lib/hydra_common/composite_automaton.h>

#include <yt/yt/core/logging/logger_owner.h>

namespace NYT::NTransactionSupervisor {

////////////////////////////////////////////////////////////////////////////////

template <class TTransaction>
class TTransactionManagerBase
    : public virtual NLogging::TLoggerOwner
{
public:
    void RegisterTransactionActionHandlers(
        const TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
        const TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
        const TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor);

    void RegisterTransactionActionHandlers(
        const TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
        const TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
        const TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor,
        const TTransactionSerializeActionHandlerDescriptor<TTransaction>& serializeActionDescriptor);

protected:
    THashMap<TString, TTransactionPrepareActionHandler<TTransaction>> PrepareActionHandlerMap_;
    THashMap<TString, TTransactionCommitActionHandler<TTransaction>> CommitActionHandlerMap_;
    THashMap<TString, TTransactionAbortActionHandler<TTransaction>> AbortActionHandlerMap_;
    THashMap<TString, TTransactionSerializeActionHandler<TTransaction>> SerializeActionHandlerMap_;

    void RunPrepareTransactionActions(TTransaction* transaction, const TTransactionPrepareOptions& options);
    void RunCommitTransactionActions(TTransaction* transaction, const TTransactionCommitOptions& options);
    void RunAbortTransactionActions(TTransaction* transaction, const TTransactionAbortOptions& options);
    void RunSerializeTransactionActions(TTransaction* transaction);
};

////////////////////////////////////////////////////////////////////////////////

//! Maintains a set of transaction ids of bounded capacity.
//! Expires old ids in FIFO order.
class TTransactionIdPool
{
public:
    explicit TTransactionIdPool(int maxSize);
    void Register(TTransactionId id);
    bool IsRegistered(TTransactionId id) const;

private:
    const int MaxSize_;
    THashSet<TTransactionId> IdSet_;
    TRingQueue<TTransactionId> IdQueue_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionSupervisor

#define TRANSACTION_MANAGER_DETAIL_INL_H_
#include "transaction_manager_detail-inl.h"
#undef TRANSACTION_MANAGER_DETAIL_INL_H_
