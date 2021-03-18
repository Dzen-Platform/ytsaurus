#pragma once

#include <yt/yt/core/misc/public.h>

#include <yt/yt/client/table_client/public.h>

namespace NYT::NTableServer {

////////////////////////////////////////////////////////////////////////////////

using TTableId = NTableClient::TTableId;

DECLARE_REFCOUNTED_CLASS(TTableManager)

using TInternedTableSchema = TInternedObject<NTableClient::TTableSchema>;
using TTableSchemaRegistry = TInternRegistry<NTableClient::TTableSchema>;
using TTableSchemaRegistryPtr = TInternRegistryPtr<NTableClient::TTableSchema>;

class TTableNode;
class TReplicatedTableNode;

template <class TImpl>
class TTableNodeTypeHandlerBase;
class TTableNodeTypeHandler;
class TReplicatedTableNodeTypeHandler;

DECLARE_REFCOUNTED_CLASS(TSharedTableSchema);
DECLARE_REFCOUNTED_CLASS(TSharedTableSchemaRegistry);
DECLARE_REFCOUNTED_CLASS(TVirtualStaticTable);
DECLARE_REFCOUNTED_CLASS(TReplicatedTableOptions);
DECLARE_REFCOUNTED_CLASS(TTabletBalancerConfig);
DECLARE_REFCOUNTED_CLASS(TPartitionConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableServer

