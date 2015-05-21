#pragma once

#include "private.h"
#include "tcp_dispatcher.h"

#include <core/misc/error.h>
#include <core/misc/address.h>

#include <core/concurrency/ev_scheduler_thread.h>
#include <core/concurrency/event_count.h>

#include <util/thread/lfqueue.h>

#include <contrib/libev/ev++.h>

#include <atomic>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

TNetworkAddress GetUnixDomainAddress(const Stroka& name);
TNetworkAddress GetLocalBusAddress(int port);
bool IsLocalServiceAddress(const Stroka& address);

////////////////////////////////////////////////////////////////////////////////

struct IEventLoopObject
    : public virtual TRefCounted
{
    virtual void SyncInitialize() = 0;
    virtual void SyncFinalize() = 0;
    virtual Stroka GetLoggingId() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IEventLoopObject)

////////////////////////////////////////////////////////////////////////////////

class TTcpDispatcherThread
    : public NConcurrency::TEVSchedulerThread
{
public:
    explicit TTcpDispatcherThread(const Stroka& threadName);

    const ev::loop_ref& GetEventLoop() const;

    TFuture<void> AsyncRegister(IEventLoopObjectPtr object);
    TFuture<void> AsyncUnregister(IEventLoopObjectPtr object);

    TTcpDispatcherStatistics& Statistics(ETcpInterfaceType interfaceType);

private:
    friend class TTcpDispatcherInvokerQueue;

    TEnumIndexedVector<TTcpDispatcherStatistics, ETcpInterfaceType> Statistics_;
    yhash_set<IEventLoopObjectPtr> Objects_;


    void DoRegister(IEventLoopObjectPtr object);
    void DoUnregister(IEventLoopObjectPtr object);

};

DEFINE_REFCOUNTED_TYPE(TTcpDispatcherThread)

////////////////////////////////////////////////////////////////////////////////

class TTcpDispatcher::TImpl
{
public:
    static TImpl* Get();

    void Shutdown();

    TTcpDispatcherStatistics GetStatistics(ETcpInterfaceType interfaceType) const;

    TTcpDispatcherThreadPtr GetServerThread();
    TTcpDispatcherThreadPtr GetClientThread();

private:
    friend TTcpDispatcher;

    TImpl();

    TTcpDispatcherThreadPtr ServerThread_;

    std::vector<TTcpDispatcherThreadPtr> ClientThreads_;
    std::atomic<size_t> CurrentClientThreadIndex_ = {0};

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
