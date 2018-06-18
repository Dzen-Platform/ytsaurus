#pragma once

#include "http.h"

#include <mapreduce/yt/interface/common.h>

#include <util/generic/maybe.h>
#include <util/str_stl.h>

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

struct TAuth
{
    TString ServerName;
    TString Token;
};

bool operator==(const TAuth& lhs, const TAuth& rhs);
bool operator!=(const TAuth& lhs, const TAuth& rhs);

///////////////////////////////////////////////////////////////////////////////

TTransactionId StartTransaction(
    const TAuth& auth,
    const TTransactionId& parentId,
    const TMaybe<TDuration>& timeout,
    bool pingAncestors,
    const TMaybe<TString>& title,
    const TMaybe<TNode>& attributes);

void PingTransaction(
    const TAuth& auth,
    const TTransactionId& transactionId);

void AbortTransaction(
    const TAuth& auth,
    const TTransactionId& transactionId);

void CommitTransaction(
    const TAuth& auth,
    const TTransactionId& transactionId);

////////////////////////////////////////////////////////////////////////////////

bool ParseBoolFromResponse(const TString& response);

TGUID ParseGuidFromResponse(const TString& response);

TRichYPath CanonizePath(
    const TAuth& auth, const TRichYPath& path);

TVector<TRichYPath> CanonizePaths(
    const TAuth& auth, const TVector<TRichYPath>& paths);

////////////////////////////////////////////////////////////////////////////////

TString GetProxyForHeavyRequest(const TAuth& auth);

TString RetryRequest(
    const TAuth& auth,
    THttpHeader& header,
    const TMaybe<TStringBuf>& body = Nothing(),
    bool isHeavy = false,
    bool isOperation = false);

void LogRequestError(
    const TString& requestId,
    const THttpHeader& header,
    const TString& message,
    const TString& attemptDescription);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

template <>
struct THash<NYT::TAuth> {
    size_t operator()(const NYT::TAuth& auth) const
    {
        return CombineHashes(THash<TString>()(auth.ServerName),
                             THash<TString>()(auth.Token));
    }
};
