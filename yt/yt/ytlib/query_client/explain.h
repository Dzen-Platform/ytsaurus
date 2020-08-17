#pragma once

#include "executor.h"

#include "public.h"

#include "query_preparer.h"

#include <yt/core/yson/string.h>

#include <yt/client/api/client.h>

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

NYson::TYsonString BuildExplainQueryYson(
    NApi::NNative::IConnectionPtr connection,
    const TString& queryString,
    const std::unique_ptr<TPlanFragment>& fragment,
    TStringBuf udfRegistryPath,
    const NApi::TExplainQueryOptions& options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient

