#include "helpers.h"

#include "config.h"

#include <mapreduce/yt/interface/serialize.h>
#include <mapreduce/yt/interface/fluent.h>

#include <mapreduce/yt/node/node_builder.h>
#include <mapreduce/yt/node/node_visitor.h>

#include <library/yson/parser.h>
#include <library/yson/writer.h>
#include <library/yson/json_writer.h>

#include <library/json/json_reader.h>
#include <library/json/json_value.h>

#include <util/stream/input.h>
#include <util/stream/output.h>
#include <util/stream/str.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TString NodeListToYsonString(const TNode::TListType& nodes)
{
    TStringStream stream;
    TYsonWriter writer(&stream, YF_BINARY, YT_LIST_FRAGMENT);
    auto list = BuildYsonListFluently(&writer);
    for (const auto& node : nodes) {
        list.Item().Value(node);
    }
    return stream.Str();
}

TNode PathToNode(const TRichYPath& path)
{
    TNode result;
    TNodeBuilder builder(&result);
    Serialize(path, &builder);
    return result;
}

TNode PathToParamNode(const TRichYPath& path)
{
    return TNode()("path", PathToNode(path));
}

TString AttributesToYsonString(const TNode& node)
{
    return BuildYsonStringFluently().BeginMap()
        .Item("attributes").Value(node)
    .EndMap();
}

TString AttributeFilterToYsonString(const TAttributeFilter& filter)
{
    return BuildYsonStringFluently().BeginMap()
        .Item("attributes").Value(filter)
    .EndMap();
}

TNode NodeFromTableSchema(const TTableSchema& schema)
{
    TNode result;
    TNodeBuilder builder(&result);
    Serialize(schema, &builder);
    return result;
}

void MergeNodes(TNode& dst, const TNode& src)
{
    if (dst.IsMap() && src.IsMap()) {
        auto& dstMap = dst.AsMap();
        const auto& srcMap = src.AsMap();
        for (const auto& srcItem : srcMap) {
            const auto& key = srcItem.first;
            auto dstItem = dstMap.find(key);
            if (dstItem != dstMap.end()) {
                MergeNodes(dstItem->second, srcItem.second);
            } else {
                dstMap[key] = srcItem.second;
            }
        }
    } else {
        if (dst.GetType() == src.GetType() && src.HasAttributes()) {
            auto attributes = dst.GetAttributes();
            MergeNodes(attributes, src.GetAttributes());
            dst = src;
            dst.Attributes() = attributes;
        } else {
            dst = src;
        }
    }
}

TYPath AddPathPrefix(const TYPath& path)
{
    if (path.StartsWith("//") || path.StartsWith("#")) {
        return path;
    }
    return TConfig::Get()->Prefix + path;
}

TString GetWriteTableCommand()
{
    return TConfig::Get()->ApiVersion == "v2" ? "write" : "write_table";
}

TString GetReadTableCommand()
{
    return TConfig::Get()->ApiVersion == "v2" ? "read" : "read_table";
}

TString GetWriteFileCommand()
{
    return TConfig::Get()->ApiVersion == "v2" ? "upload" : "write_file";
}

TString GetReadFileCommand()
{
    return TConfig::Get()->ApiVersion == "v2" ? "download" : "read_file";
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
