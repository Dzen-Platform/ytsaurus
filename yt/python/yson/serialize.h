#pragma once

#include <yt/core/misc/optional.h>
#include <yt/core/misc/ref.h>
#include <yt/core/misc/small_vector.h>

#include <yt/core/yson/consumer.h>

#include <yt/core/ytree/public.h>

#include <Objects.hxx> // pycxx

#include <queue>
#include <stack>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TPathPart
{
    TStringBuf Key;
    int Index = -1;
    bool InAttributes = false;
};

struct TContext
{
    SmallVector<TPathPart, 2> PathParts;
    std::optional<size_t> RowIndex;

    void Push(TStringBuf& key)
    {
        TPathPart pathPart;
        pathPart.Key = key;
        PathParts.push_back(pathPart);
    }

    void Push(int index)
    {
        TPathPart pathPart;
        pathPart.Index = index;
        PathParts.push_back(pathPart);
    }

    void PushAttributesStarted()
    {
        TPathPart pathPart;
        pathPart.InAttributes = true;
        PathParts.push_back(pathPart);
    }

    void Pop()
    {
        PathParts.pop_back();
    }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

// This methods allow use methods ConvertTo* with Py::Object.
void Serialize(
    const Py::Object& obj,
    NYson::IYsonConsumer* consumer,
#if PY_MAJOR_VERSION >= 3
    const std::optional<TString>& encoding = std::make_optional(TString("utf-8")),
#else
    const std::optional<TString>& encoding = std::nullopt,
#endif
    bool ignoreInnerAttributes = false,
    NYson::EYsonType ysonType = NYson::EYsonType::Node,
    int depth = 0,
    TContext* context = nullptr);

void Deserialize(Py::Object& obj, NYTree::INodePtr node, const std::optional<TString>& encoding = std::nullopt);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

Py::Object CreateYsonObject(const std::string& className, const Py::Object& object, const Py::Object& attributes);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
