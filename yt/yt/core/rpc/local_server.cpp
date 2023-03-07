#include "local_server.h"
#include "server_detail.h"
#include "private.h"

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

class TLocalServer
    : public TServerBase
{
public:
    TLocalServer()
        : TServerBase(RpcServerLogger)
    { }
};

IServerPtr CreateLocalServer()
{
    return New<TLocalServer>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
