#pragma once

#include "public.h"

#include <yt/core/misc/optional.h>

#include <yt/core/http/public.h>

#include <yt/core/ytree/public.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

std::optional<TString> GatherHeader(const NHttp::THeadersPtr& headers, const TString& headerName);

NYTree::IMapNodePtr ParseQueryString(TStringBuf queryString);

void FixupNodesWithAttributes(const NYTree::IMapNodePtr& node);

NYTree::IMapNodePtr HideSecretParameters(const TString& commandName, NYTree::IMapNodePtr parameters);

struct TPythonWrapperVersion
{
    int Major = 0;
    int Minor = 0;
    int Patch = 0;
};

std::optional<TPythonWrapperVersion> DetectPythonWrapper(const TString& userAgent);

std::optional<i64> DetectJavaIceberg(const TString& userAgent);

std::optional<i64> DetectGo(const TString& userAgent);

bool IsClientBuggy(const NHttp::IRequestPtr& req);

////////////////////////////////////////////////////////////////////////////////

struct TNetworkStatistics
{
    i64 TotalRxBytes = 0;
    i64 TotalTxBytes = 0;
};

std::optional<TNetworkStatistics> GetNetworkStatistics();

////////////////////////////////////////////////////////////////////////////////

void ReplyError(const NHttp::IResponseWriterPtr& response, const TError& error);

void ProcessDebugHeaders(
    const NHttp::IRequestPtr& request,
    const NHttp::IResponseWriterPtr& response,
    const TCoordinatorPtr& coordinator);

void RedirectToDataProxy(
    const NHttp::IRequestPtr& request,
    const NHttp::IResponseWriterPtr& response,
    const TCoordinatorPtr& coordinator);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
