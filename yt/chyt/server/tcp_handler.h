#pragma once

#include "private.h"

#include <Server/IServer.h>

#include <Poco/Net/TCPServerConnectionFactory.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

Poco::Net::TCPServerConnectionFactory::Ptr CreateTcpHandlerFactory(THost* host, DB::IServer& server);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
