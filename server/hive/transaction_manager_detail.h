#pragma once

#include "public.h"
#include "helpers.h"

#include <yt/server/hydra/composite_automaton.h>

namespace NYT {
namespace NHiveServer {

////////////////////////////////////////////////////////////////////////////////

template <class TTransaction>
class TTransactionManagerBase
{
public:
    void RegisterPrepareActionHandler(const TTransactionPrepareActionHandlerDescriptor<TTransaction>& descriptor);
    void RegisterCommitActionHandler(const TTransactionCommitActionHandlerDescriptor<TTransaction>& descriptor);
    void RegisterAbortActionHandler(const TTransactionAbortActionHandlerDescriptor<TTransaction>& descriptor);

protected:
    yhash_map<Stroka, TTransactionPrepareActionHandler<TTransaction>> PrepareActionHandlerMap_;
    yhash_map<Stroka, TTransactionCommitActionHandler<TTransaction>> CommitActionHandlerMap_;
    yhash_map<Stroka, TTransactionAbortActionHandler<TTransaction>> AbortActionHandlerMap_;


    void RunPrepareTransactionActions(TTransaction* transaction, bool persistent);
    void RunCommitTransactionActions(TTransaction* transaction);
    void RunAbortTransactionActions(TTransaction* transaction);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveServer
} // namespace NYT

#define TRANSACTION_MANAGER_DETAIL_INL_H_
#include "transaction_manager_detail-inl.h"
#undef TRANSACTION_MANAGER_DETAIL_INL_H_
