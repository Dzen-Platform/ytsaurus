#pragma once

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/yson/public.h>

#include <Objects.hxx> // pycxx

#include <util/stream/input.h>
#include <util/generic/string.h>

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

Py::Object ParseLazyYson(
    IInputStream* inputStream,
    const std::optional<TString>& encoding,
    bool alwaysCreateAttributes,
    NYson::EYsonType ysonType);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
