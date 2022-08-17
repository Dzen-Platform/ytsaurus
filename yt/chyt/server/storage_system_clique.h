#pragma once

#include <yt/yt/client/misc/public.h>

#include <Storages/IStorage_fwd.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

DB::StoragePtr CreateStorageSystemClique(
    IDiscoveryPtr discovery,
    TGuid InstanceId_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
