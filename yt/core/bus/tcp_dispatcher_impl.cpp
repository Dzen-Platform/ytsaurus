#include "stdafx.h"
#include "tcp_dispatcher_impl.h"
#include "config.h"
#include "tcp_connection.h"

#include <core/misc/address.h>

#ifndef _win_
    #include <sys/socket.h>
    #include <sys/un.h>
#endif

namespace NYT {
namespace NBus {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = BusLogger;
static const int ThreadCount = 8;

////////////////////////////////////////////////////////////////////////////////

TNetworkAddress GetUnixDomainAddress(const Stroka& name)
{
#ifdef _win_
    THROW_ERROR_EXCEPTION("Local bus transport is not supported under this platform");
#else
    sockaddr_un sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));
    sockAddr.sun_family = AF_UNIX;
    strncpy(sockAddr.sun_path + 1, ~name, name.length());
    return TNetworkAddress(
        *reinterpret_cast<sockaddr*>(&sockAddr),
        sizeof (sockAddr.sun_family) +
        sizeof (char) +
        name.length());
#endif
}

TNetworkAddress GetLocalBusAddress(int port)
{
    auto name = Format("yt-local-bus-%v", port);
    return GetUnixDomainAddress(name);
}

bool IsLocalServiceAddress(const Stroka& address)
{
#ifdef _linux_
    TStringBuf hostName;
    int port;
    try {
        ParseServiceAddress(address, &hostName, &port);
        return hostName == TAddressResolver::Get()->GetLocalHostName();
    } catch (...) {
        return false;
    }
#else
    // Domain sockets are only supported for Linux.
    UNUSED(address);
    return false;
#endif
}

////////////////////////////////////////////////////////////////////////////////

TTcpDispatcherThread::TTcpDispatcherThread(const Stroka& threadName)
    : TEVSchedulerThread(threadName, false)
{ }

const ev::loop_ref& TTcpDispatcherThread::GetEventLoop() const
{
    return EventLoop;
}

TFuture<void> TTcpDispatcherThread::AsyncRegister(IEventLoopObjectPtr object)
{
    LOG_DEBUG("Object registration enqueued (%v)", object->GetLoggingId());

    return BIND(&TTcpDispatcherThread::DoRegister, MakeStrong(this), object)
        .AsyncVia(GetInvoker())
        .Run();
}

TFuture<void> TTcpDispatcherThread::AsyncUnregister(IEventLoopObjectPtr object)
{
    LOG_DEBUG("Object unregistration enqueued (%v)", object->GetLoggingId());

    return BIND(&TTcpDispatcherThread::DoUnregister, MakeStrong(this), object)
        .AsyncVia(GetInvoker())
        .Run();
}

TTcpDispatcherStatistics& TTcpDispatcherThread::Statistics(ETcpInterfaceType interfaceType)
{
    return Statistics_[interfaceType];
}

void TTcpDispatcherThread::DoRegister(IEventLoopObjectPtr object)
{
    object->SyncInitialize();
    YCHECK(Objects_.insert(object).second);

    LOG_DEBUG("Object registered (%v)", object->GetLoggingId());
}

void TTcpDispatcherThread::DoUnregister(IEventLoopObjectPtr object)
{
    object->SyncFinalize();
    YCHECK(Objects_.erase(object) == 1);

    LOG_DEBUG("Object unregistered (%v)", object->GetLoggingId());
}

////////////////////////////////////////////////////////////////////////////////

TTcpDispatcher::TImpl::TImpl()
{
    ServerThread_ = New<TTcpDispatcherThread>("BusServer");
    ServerThread_->Start();

    for (int index = 0; index < ThreadCount; ++index) {
        auto thread = New<TTcpDispatcherThread>(Format("BusClient:%v", index));
        thread->Start();
        ClientThreads_.push_back(thread);
    }
}

TTcpDispatcher::TImpl* TTcpDispatcher::TImpl::Get()
{
    return TTcpDispatcher::Get()->Impl_.get();
}

void TTcpDispatcher::TImpl::Shutdown()
{
    for (auto& thread : ClientThreads_) {
        thread->Shutdown();
    }
}

TTcpDispatcherStatistics TTcpDispatcher::TImpl::GetStatistics(ETcpInterfaceType interfaceType) const
{
    // This is racy but should be OK as an approximation.
    TTcpDispatcherStatistics result;
    for (auto& thread : ClientThreads_) {
        result += thread->Statistics(interfaceType);
    }
    return result;
}

TTcpDispatcherThreadPtr TTcpDispatcher::TImpl::GetServerThread()
{
    return ServerThread_;
}

TTcpDispatcherThreadPtr TTcpDispatcher::TImpl::GetClientThread()
{
    size_t index = CurrentClientThreadIndex_++ % ThreadCount;
    return ClientThreads_[index];
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
