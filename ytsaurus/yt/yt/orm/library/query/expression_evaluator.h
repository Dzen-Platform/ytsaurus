#pragma once

#include "public.h"

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/core/yson/public.h>

#include <yt/yt/library/query/base/public.h>

namespace NYT::NOrm::NQuery {

////////////////////////////////////////////////////////////////////////////////

struct IExpressionEvaluator
    : public TRefCounted
{
    virtual TErrorOr<NQueryClient::TValue> Evaluate(
        const std::vector<NYson::TYsonStringBuf>& attributeYsons,
        NTableClient::TRowBufferPtr rowBuffer = nullptr) = 0;

    //! Shortcut for the input vector of size 1.
    virtual TErrorOr<NQueryClient::TValue> Evaluate(
        const NYson::TYsonStringBuf& attributeYson,
        NTableClient::TRowBufferPtr rowBuffer = nullptr) = 0;

    virtual const TString& GetQuery() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IExpressionEvaluator)

////////////////////////////////////////////////////////////////////////////////

//! Thread-safe; exception-safe.
IExpressionEvaluatorPtr CreateExpressionEvaluator(
    TString query,
    std::vector<TString> attributePaths = {""});

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NOrm::NQuery
