#pragma once

#include "yt/core/misc/public.h"
#include "yt/core/net/address.h"

#include <util/network/init.h>

namespace NYT {
namespace NNet {

////////////////////////////////////////////////////////////////////////////////

//! Create NONBLOCKING server socket, set IPV6_ONLY and REUSEADD flags.
SOCKET CreateTcpServerSocket();
SOCKET CreateUnixServerSocket();
SOCKET CreateTcpClientSocket(int family);
SOCKET CreateUnixClientSocket();

//! Start connect on the socket. Any errors other than EWOULDBLOCK,
//! EAGAIN and EINPROGRESS are thrown as exceptions.
int ConnectSocket(SOCKET clientSocket, const TNetworkAddress& address);

//! Try binding socket to address. Error is thrown as exception.
void BindSocket(SOCKET serverSocket, const TNetworkAddress& address);

//! Try to accept client on non-blocking socket. Any errors other than
//! EWOULDBLOCK or EAGAIN are thrown as exceptions.
//! Returned socket has CLOEXEC and NONBLOCK flags set.
int AcceptSocket(SOCKET serverSocket, TNetworkAddress* clientAddress);

void ListenSocket(SOCKET serverSocket, int backlog);

int GetSocketError(SOCKET socket);

TNetworkAddress GetSocketName(SOCKET socket);
TNetworkAddress GetSocketPeer(SOCKET socket);

void SetSocketPriority(SOCKET socket, int priority);
void SetSocketNoDelay(SOCKET socket);
void SetSocketKeepAlive(SOCKET socket);
void SetSocketEnableQuickAck(SOCKET socket);

////////////////////////////////////////////////////////////////////////////////

} // namespace NNet
} // namespace NYT
