#pragma once

#include "public.h"
#include "utf8_decoder.h"

#include <core/ytree/public.h>
#include <core/ytree/tree_builder.h>

#include <stack>
#include <queue>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EJsonCallbacksNodeType,
    (List)
    (Map)
);

class TJsonCallbacks
{
public:
    TJsonCallbacks();
    TJsonCallbacks(const TUtf8Transcoder& utf8Transcoder, i64 memoryLimit);

    void OnStringScalar(const TStringBuf& value);
    void OnInt64Scalar(i64 value);
    void OnUint64Scalar(ui64 value);
    void OnDoubleScalar(double value);
    void OnBooleanScalar(bool value);
    void OnEntity();
    void OnBeginList();
    void OnEndList();
    void OnBeginMap();
    void OnKeyedItem(const TStringBuf& key);
    void OnEndMap();

    bool HasFinishedNodes() const;
    NYTree::INodePtr ExtractFinishedNode();

private:
    // Memory accounted approximately
    void AccountMemory(i64 memory);
    void OnItemStarted();
    void OnItemFinished();

    TUtf8Transcoder Utf8Transcoder_;
    i64 ConsumedMemory_;
    i64 MemoryLimit_;

    using ENodeType = EJsonCallbacksNodeType;
    std::stack<ENodeType> Stack_;

    std::unique_ptr<NYTree::ITreeBuilder> TreeBuilder_;
    std::queue<NYTree::INodePtr> FinishedNodes_;
    std::queue<i64> NodesMemory_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
