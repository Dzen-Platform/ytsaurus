#pragma once

#include "requests.h"

#include <util/system/thread.h>
#include <util/datetime/base.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TPingableTransaction
{
public:
    TPingableTransaction(
        const TAuth& auth,
        const TTransactionId& parentId,
        const TMaybe<TDuration>& timeout = Nothing(),
        bool pingAncestors = false,
        const TMaybe<TNode>& attributes = Nothing());

    ~TPingableTransaction();

    const TTransactionId GetId() const;

    void Commit();
    void Abort();

private:
    TAuth Auth_;
    TTransactionId TransactionId_;

    volatile bool Running_;
    TThread Thread_;

    void Stop(bool commit);

    void Pinger();
    static void* Pinger(void* opaque);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
