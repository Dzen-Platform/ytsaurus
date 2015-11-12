#pragma once

#include "ypath_client.h"

#include <yt/core/ytree/ypath.pb.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

struct TYPathProxy
{
    static Stroka GetServiceName()
    {
        return "Node";
    }

    DEFINE_YPATH_PROXY_METHOD(NProto, GetKey);
    DEFINE_YPATH_PROXY_METHOD(NProto, Get);
    DEFINE_YPATH_PROXY_METHOD(NProto, List);
    DEFINE_YPATH_PROXY_METHOD(NProto, Exists);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Set);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Remove);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
