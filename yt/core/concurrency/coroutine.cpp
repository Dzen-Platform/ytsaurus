#include "coroutine.h"

#include <yt/core/misc/common.h>

namespace NYT {
namespace NConcurrency {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

TCoroutineBase::TCoroutineBase()
    : Completed_(false)
    , CoroutineStack_(CreateExecutionStack(EExecutionStack::Small))
    , CoroutineContext_(CreateExecutionContext(CoroutineStack_.get(), &TCoroutineBase::Trampoline))
{ }

TCoroutineBase::TCoroutineBase(TCoroutineBase&& other)
    : Completed_(other.Completed_)
    , CallerContext_(std::move(other.CallerContext_))
    , CoroutineStack_(std::move(other.CoroutineStack_))
    , CoroutineContext_(std::move(other.CoroutineContext_))
{
    other.Completed_ = true;
}

TCoroutineBase::~TCoroutineBase()
{ }

void TCoroutineBase::Trampoline(void* opaque)
{
    auto* coroutine = reinterpret_cast<TCoroutineBase*>(opaque);
    YASSERT(coroutine);

    try {
        coroutine->Invoke();
    } catch (...) {
        coroutine->CoroutineException_ = std::current_exception();
    }

    coroutine->Completed_ = true;
    coroutine->JumpToCaller();

    YUNREACHABLE();
}

void TCoroutineBase::JumpToCaller()
{
    SwitchExecutionContext(&CoroutineContext_, &CallerContext_, nullptr);
}

void TCoroutineBase::JumpToCoroutine()
{
    SwitchExecutionContext(&CallerContext_, &CoroutineContext_, this);

    if (CoroutineException_) {
        std::exception_ptr exception;
        std::swap(exception, CoroutineException_);
        std::rethrow_exception(std::move(exception));
    }
}

bool TCoroutineBase::IsCompleted() const
{
    return Completed_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NConcurrency
} // namespace NYT
