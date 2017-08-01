#pragma once

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/ypath_client.h>

namespace NYT {
namespace NBlackbox {

////////////////////////////////////////////////////////////////////////////////

template <class T>
TErrorOr<T> GetByYPath(const NYTree::INodePtr& node, const NYPath::TYPath& path)
{
    try {
        auto child = NYTree::FindNodeByYPath(node, path);
        if (!child) {
            return TError("Missing %v", path);
        }
        return NYTree::ConvertTo<T>(std::move(child));
    } catch (const std::exception& ex) {
        return TError("Unable to extract %v", path) << ex;
    }
}

TString ComputeMD5(const TString& token);

////////////////////////////////////////////////////////////////////////////////

} // namespace NBlackbox
} // namespace NYT
