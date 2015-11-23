#include "stdafx.h"
#include "io_dispatcher.h"
#include "io_dispatcher_impl.h"

#include <core/misc/singleton.h>

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

TIODispatcher::TIODispatcher()
    : Impl_(New<TIODispatcher::TImpl>())
{ }

TIODispatcher::~TIODispatcher()
{ }

TIODispatcher* TIODispatcher::Get()
{
    return Singleton<TIODispatcher>();
}

IInvokerPtr TIODispatcher::GetInvoker()
{
    if (Y_UNLIKELY(!Impl_->IsStarted())) {
        Impl_->Start();
    }
    return Impl_->GetInvoker();
}

const ev::loop_ref& TIODispatcher::GetEventLoop()
{
    if (Y_UNLIKELY(!Impl_->IsStarted())) {
        Impl_->Start();
    }
    return Impl_->GetEventLoop();
}

void TIODispatcher::StaticShutdown()
{
    Get()->Shutdown();
}

void TIODispatcher::Shutdown()
{
    return Impl_->Shutdown();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
