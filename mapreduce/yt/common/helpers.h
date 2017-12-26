#pragma once

#include <mapreduce/yt/node/node_io.h> // backward compatibility

#include <mapreduce/yt/interface/node.h>
#include <mapreduce/yt/interface/common.h>
#include <library/yson/public.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TString NodeListToYsonString(const TNode::TListType& nodes);

TNode PathToNode(const TRichYPath& path);
TNode PathToParamNode(const TRichYPath& path);

TString AttributesToYsonString(const TNode& attributes);

TString AttributeFilterToYsonString(const TAttributeFilter& filter);

TNode NodeFromTableSchema(const TTableSchema& schema);

void MergeNodes(TNode& dst, const TNode& src);

TYPath AddPathPrefix(const TYPath& path);

TString GetWriteTableCommand();
TString GetReadTableCommand();
TString GetWriteFileCommand();
TString GetReadFileCommand();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
