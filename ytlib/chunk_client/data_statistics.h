#pragma once

#include <yt/ytlib/chunk_client/data_statistics.pb.h>

#include <yt/core/yson/public.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

TDataStatistics& operator += (TDataStatistics& lhs, const TDataStatistics& rhs);
TDataStatistics  operator +  (const TDataStatistics& lhs, const TDataStatistics& rhs);

TDataStatistics& operator -= (TDataStatistics& lhs, const TDataStatistics& rhs);
TDataStatistics  operator -  (const TDataStatistics& lhs, const TDataStatistics& rhs);

bool operator==  (const TDataStatistics& lhs, const TDataStatistics& rhs);

void Serialize(const TDataStatistics& statistics, NYson::IYsonConsumer* consumer);
void Deserialize(TDataStatistics& value, NYTree::INodePtr node);

const TDataStatistics& ZeroDataStatistics();

Stroka ToString(const TDataStatistics& statistics);

}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

