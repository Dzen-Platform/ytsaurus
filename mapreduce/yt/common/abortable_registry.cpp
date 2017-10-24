#include "abortable_registry.h"

#include "mapreduce/yt/client/operation.h"

#include <mapreduce/yt/common/log.h>

#include <util/generic/singleton.h>

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

TTransactionAbortable::TTransactionAbortable(const TAuth& auth, const TTransactionId& transactionId)
    : Auth_(auth)
    , TransactionId_(transactionId)
{ }

void TTransactionAbortable::Abort()
{
    AbortTransaction(Auth_, TransactionId_);
}

TString TTransactionAbortable::GetType() const
{
    return "transaction";
}

////////////////////////////////////////////////////////////////////////////////

TOperationAbortable::TOperationAbortable(const TAuth& auth, const TOperationId& operationId)
    : Auth_(auth)
    , OperationId_(operationId)
{ }


void TOperationAbortable::Abort()
{
    AbortOperation(Auth_, OperationId_);
}

TString TOperationAbortable::GetType() const
{
    return "operation";
}

////////////////////////////////////////////////////////////////////////////////

void TAbortableRegistry::AbortAllAndBlockForever()
{
    auto guard = Guard(Lock_);

    for (const auto& entry : ActiveAbortables_) {
        const auto& id = entry.first;
        const auto& abortable = entry.second;
        try {
            abortable->Abort();
        } catch (yexception& ex) {
            LOG_ERROR("Exception while aborting %s %s: %s",
                      ~abortable->GetType(),
                      ~GetGuidAsString(id),
                      ex.what());
        }
    }

    Running_ = false;
}

void TAbortableRegistry::Add(const TGUID& id, IAbortablePtr abortable)
{
    auto guard = Guard(Lock_);

    if (!Running_) {
        Sleep(TDuration::Max());
    }

    ActiveAbortables_[id] = abortable;
}

void TAbortableRegistry::Remove(const TGUID& id)
{
    auto guard = Guard(Lock_);

    if (!Running_) {
        Sleep(TDuration::Max());
    }

    ActiveAbortables_.erase(id);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

class TRegistryHolder
{
public:
    TRegistryHolder()
        : Registry_(::MakeIntrusive<TAbortableRegistry>())
    { }

    ::TIntrusivePtr<TAbortableRegistry> Get()
    {
        return Registry_;
    }

private:
    ::TIntrusivePtr<TAbortableRegistry> Registry_;
};

} // namespace

::TIntrusivePtr<TAbortableRegistry> TAbortableRegistry::Get()
{
    return Singleton<TRegistryHolder>()->Get();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
