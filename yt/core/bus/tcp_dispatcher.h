#pragma once

#include "public.h"

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

struct TTcpDispatcherStatistics
{
    TTcpDispatcherStatistics();

    int PendingInCount;
    i64 PendingInSize;

    int PendingOutCount;
    i64 PendingOutSize;

    int ClientConnectionCount;
    int ServerConnectionCount;
};

TTcpDispatcherStatistics operator + (
    const TTcpDispatcherStatistics& lhs,
    const TTcpDispatcherStatistics& rhs);

TTcpDispatcherStatistics& operator += (
    TTcpDispatcherStatistics& lhs,
    const TTcpDispatcherStatistics& rhs);

////////////////////////////////////////////////////////////////////////////////

//! Local means UNIX domain sockets.
//! Remove means standard TCP sockets.
DEFINE_ENUM(ETcpInterfaceType,
    (Local)
    (Remote)
);

////////////////////////////////////////////////////////////////////////////////

class TTcpDispatcher
{
public:
    static TTcpDispatcher* Get();

    void Shutdown();

    TTcpDispatcherStatistics GetStatistics(ETcpInterfaceType interfaceType);

private:
    TTcpDispatcher();

    DECLARE_SINGLETON_FRIEND(TTcpDispatcher);
    friend class TTcpConnection;
    friend class TTcpClientBusProxy;
    friend class TTcpBusServerBase;
    template <class TServer>
    friend class TTcpBusServerProxy;

    class TImpl;
    std::unique_ptr<TImpl> Impl_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
