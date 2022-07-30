#pragma once

#include <yt/yt/client/api/public.h>

#include <yt/yt/client/tablet_client/public.h>

namespace NYT::NApi::NNative {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IConnection)
DECLARE_REFCOUNTED_STRUCT(IClient)
DECLARE_REFCOUNTED_STRUCT(ITransaction)
DECLARE_REFCOUNTED_CLASS(TClientCache)
DECLARE_REFCOUNTED_CLASS(TStickyGroupSizeCache)
DECLARE_REFCOUNTED_CLASS(TSyncReplicaCache)
DECLARE_REFCOUNTED_CLASS(TTabletSyncReplicaCache)

DECLARE_REFCOUNTED_STRUCT(ICellCommitSession)
DECLARE_REFCOUNTED_STRUCT(ICellCommitSessionProvider)
DECLARE_REFCOUNTED_STRUCT(ITabletCommitSession)
DECLARE_REFCOUNTED_STRUCT(ITabletRequestBatcher)

DECLARE_REFCOUNTED_CLASS(TMasterConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TMasterCacheConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TClockServersConfig)
DECLARE_REFCOUNTED_CLASS(TConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TConnectionDynamicConfig)

DECLARE_REFCOUNTED_CLASS(TJournalChunkWriterOptions)

struct TConnectionOptions;

struct TNativeTransactionStartOptions;

class TTabletSyncReplicaCache;

using TTableReplicaInfoPtrList = TCompactVector<
    NTabletClient::TTableReplicaInfoPtr,
    NTabletClient::TypicalTableReplicaCount>;

using TTableReplicaIdList = TCompactVector<
    NTabletClient::TTableReplicaId,
    NTabletClient::TypicalTableReplicaCount>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative

