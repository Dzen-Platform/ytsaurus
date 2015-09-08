#include "node.h"
#include "convert.h"
#include "node_detail.h"

#include <core/yson/async_writer.h>

namespace NYT {
namespace NYTree {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

const ENodeType TScalarTypeTraits<Stroka>::NodeType = ENodeType::String;
const ENodeType TScalarTypeTraits<i64>::NodeType = ENodeType::Int64;
const ENodeType TScalarTypeTraits<ui64>::NodeType = ENodeType::Uint64;
const ENodeType TScalarTypeTraits<double>::NodeType = ENodeType::Double;
const ENodeType TScalarTypeTraits<bool>::NodeType = ENodeType::Boolean;

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

TYPath INode::GetPath() const
{
    return GetResolver()->GetPath(const_cast<INode*>(this));
}

INodePtr IMapNode::GetChild(const Stroka& key) const
{
    auto child = FindChild(key);
    if (!child) {
        ThrowNoSuchChildKey(this, key);
    }
    return child;
}

INodePtr IListNode::GetChild(int index) const
{
    auto child = FindChild(index);
    if (!child) {
        ThrowNoSuchChildIndex(this, index);
    }
    return child;
}

int IListNode::AdjustChildIndex(int index) const
{
    int adjustedIndex = index >= 0 ? index : index + GetChildCount();
    if (adjustedIndex < 0 || adjustedIndex >= GetChildCount()) {
        ThrowNoSuchChildIndex(this, index);
    }
    return adjustedIndex;
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(INode& value, IYsonConsumer* consumer)
{
    VisitTree(&value, consumer);
}

void Deserialize(INodePtr& value, INodePtr node)
{
    value = node;
}

TYsonString ConvertToYsonStringStable(INodePtr node)
{
    TStringStream stream;
    TYsonWriter writer(&stream, EYsonFormat::Binary, EYsonType::Node);
    VisitTree(
        node,
        &writer,
        TAttributeFilter::All,
        true); // truth matters :)
    return TYsonString(stream.Str(), EYsonType::Node);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
