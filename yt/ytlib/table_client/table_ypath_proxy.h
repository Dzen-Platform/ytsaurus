#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>

#include <yt/ytlib/table_client/table_ypath.pb.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TTableYPathProxy
    : public NChunkClient::TChunkOwnerYPathProxy
{
    static Stroka GetServiceName()
    {
        return "Table";
    }

    DEFINE_YPATH_PROXY_METHOD(NProto, GetMountInfo);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Mount);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Unmount);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Freeze);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Unfreeze);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Remount);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Reshard);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Alter);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
