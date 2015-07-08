#include "public.h"

#include <core/actions/public.h>
#include <core/rpc/public.h>

#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/node_tracker_client/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

TFuture<NObjectClient::TMasterYPathProxy::TRspCreateObjectPtr> CreateChunk(
    NRpc::IChannelPtr masterChannel,
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    NObjectClient::EObjectType chunkType,
    NObjectClient::TTransactionId transactionId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
