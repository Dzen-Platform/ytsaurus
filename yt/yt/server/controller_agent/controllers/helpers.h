#pragma once

#include "private.h"

#include "data_flow_graph.h"

#include <yt/yt/server/lib/chunk_pools/chunk_stripe_key.h>

#include <yt/yt/ytlib/chunk_client/helpers.h>

#include <yt/yt/ytlib/table_client/helpers.h>

namespace NYT::NControllerAgent::NControllers {

////////////////////////////////////////////////////////////////////////////////

NChunkPools::TBoundaryKeys BuildBoundaryKeysFromOutputResult(
    const NScheduler::NProto::TOutputResult& boundaryKeys,
    const TStreamDescriptor& outputTable,
    const NTableClient::TRowBufferPtr& rowBuffer);

////////////////////////////////////////////////////////////////////////////////

NChunkClient::TDataSourceDirectoryPtr BuildDataSourceDirectoryFromInputTables(const std::vector<TInputTablePtr>& inputTables);
NChunkClient::TDataSinkDirectoryPtr BuildDataSinkDirectoryFromOutputTables(const std::vector<TOutputTablePtr>& outputTables);

////////////////////////////////////////////////////////////////////////////////

class TControllerFeatures
{
public:
    void AddTag(TString name, auto value);
    void AddSingular(TStringBuf name, double value);
    void AddSingular(const TString& name, const NYTree::INodePtr& node);
    void AddCounted(TStringBuf name, double value);
    void CalculateJobSatisticsAverage();

private:
    THashMap<TString, NYson::TYsonString> Tags_;
    THashMap<TString, double> Features_;

    friend void Serialize(const TControllerFeatures& features, NYson::IYsonConsumer* consumer);
};

void Serialize(const TControllerFeatures& features, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

NTableClient::TTableReaderOptionsPtr CreateTableReaderOptions(const NScheduler::TJobIOConfigPtr& ioConfig);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NControllers

#define HELPERS_INL_H
#include "helpers-inl.h"
#undef HELPERS_INL_H
