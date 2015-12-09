#pragma once

#include "public.h"

#include <yt/core/misc/function_traits.h>

#include <yt/core/ytree/public.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

NYson::TYsonString DoRunTool(const Stroka& toolName, const NYson::TYsonString& serializedArgument);
NYson::TYsonString DoRunToolInProcess(const Stroka& toolName, const NYson::TYsonString& serializedArgument);

////////////////////////////////////////////////////////////////////////////////

template <
    typename TTool,
    typename TArg = typename TFunctionTraits<TTool>::TArg,
    typename TResult = typename TFunctionTraits<TTool>::TResult>
TResult RunTool(
    const TArg& arg,
    std::function<NYson::TYsonString(const Stroka&, const NYson::TYsonString&)> invoker = DoRunTool);

////////////////////////////////////////////////////////////////////////////////

NYson::TYsonString ExecuteTool(const Stroka& toolName, const NYson::TYsonString& serializedArgument);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define TOOLS_INL_H_
#include "tools-inl.h"
#undef TOOLS_INL_H_
