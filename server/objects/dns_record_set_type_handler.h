#pragma once

#include "public.h"

#include <yp/server/master/public.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IObjectTypeHandler> CreateDnsRecordSetTypeHandler(NMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

