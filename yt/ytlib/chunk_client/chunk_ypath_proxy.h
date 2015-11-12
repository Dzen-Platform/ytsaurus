#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/chunk_owner_ypath.pb.h>
#include <yt/ytlib/chunk_client/chunk_ypath.pb.h>

#include <yt/ytlib/object_client/object_ypath_proxy.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TChunkYPathProxy
    : public NObjectClient::TObjectYPathProxy
{
    static Stroka GetServiceName()
    {
        return "Chunk";
    }

    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Confirm);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Seal);
    // NB: Works only for table chunks.
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NChunkClient::NProto, Fetch);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
