#pragma once

#include "private.h"

#include "data_flow_graph.h"

#include <yt/server/lib/chunk_pools/chunk_stripe_key.h>

#include <yt/server/lib/controller_agent/serialize.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/table_client/helpers.h>

#include <yt/ytlib/scheduler/config.h>

namespace NYT::NControllerAgent {

using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

template <class TSpec>
TIntrusivePtr<TSpec> ParseOperationSpec(NYTree::INodePtr specNode);

NYTree::INodePtr UpdateSpec(NYTree::INodePtr templateSpec, NYTree::INodePtr originalSpec);

////////////////////////////////////////////////////////////////////////////////

TString TrimCommandForBriefSpec(const TString& command);

////////////////////////////////////////////////////////////////////////////////

struct TUserFile
    : public NChunkClient::TUserObject
{
    TUserFile() = default;
    TUserFile(
        NYPath::TRichYPath path,
        std::optional<NObjectClient::TTransactionId> transactionId,
        bool layer);

    std::shared_ptr<NYTree::IAttributeDictionary> Attributes;
    TString FileName;
    std::vector<NChunkClient::NProto::TChunkSpec> ChunkSpecs;
    i64 ChunkCount = -1;
    bool Executable = false;
    NYson::TYsonString Format;
    NTableClient::TTableSchema Schema;
    bool Dynamic = false;
    bool Layer = false;
    // This field is used only during file size validation only for table chunks with column selectors.
    std::vector<NChunkClient::TInputChunkPtr> Chunks;

    void Persist(const TPersistenceContext& context);
};

////////////////////////////////////////////////////////////////////////////////

NChunkPools::TBoundaryKeys BuildBoundaryKeysFromOutputResult(
    const NScheduler::NProto::TOutputResult& boundaryKeys,
    const TEdgeDescriptor& outputTable,
    const NTableClient::TRowBufferPtr& rowBuffer);

void BuildFileSpecs(NScheduler::NProto::TUserJobSpec* jobSpec, const std::vector<TUserFile>& files);

////////////////////////////////////////////////////////////////////////////////

NChunkClient::TDataSourceDirectoryPtr BuildDataSourceDirectoryFromInputTables(const std::vector<TInputTablePtr>& inputTables);
NChunkClient::TDataSourceDirectoryPtr BuildIntermediateDataSourceDirectory();

void SetDataSourceDirectory(NScheduler::NProto::TSchedulerJobSpecExt* jobSpec, const NChunkClient::TDataSourceDirectoryPtr& dataSourceDirectory);

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TAvgSummary
{
public:
    DEFINE_BYVAL_RO_PROPERTY(T, Sum);
    DEFINE_BYVAL_RO_PROPERTY(i64, Count);
    DEFINE_BYVAL_RO_PROPERTY(std::optional<T>, Avg);

public:
    TAvgSummary();
    TAvgSummary(T sum, i64 count);

    void AddSample(T sample);

    void Persist(const TPersistenceContext& context);

private:
    std::optional<T> CalcAvg();
};

////////////////////////////////////////////////////////////////////////////////

ELegacyLivePreviewMode ToLegacyLivePreviewMode(std::optional<bool> enableLegacyLivePreview);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent

#define HELPERS_INL_H_
#include "helpers-inl.h"
#undef HELPERS_INL_H_
