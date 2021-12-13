#include "py_helpers.h"

#include "client.h"
#include "operation.h"
#include "transaction.h"

#include <mapreduce/yt/interface/client.h>
#include <mapreduce/yt/interface/fluent.h>

#include <mapreduce/yt/common/helpers.h>

#include <library/cpp/yson/node/node_io.h>

#include <util/generic/hash_set.h>

namespace NYT {

using namespace NDetail;

////////////////////////////////////////////////////////////////////////////////

IStructuredJobPtr ConstructJob(const TString& jobName, const TString& state)
{
    auto node = TNode();
    if (!state.empty()) {
        node = NodeFromYsonString(state);
    }
    return TJobFactory::Get()->GetConstructingFunction(jobName.data())(node);
}

TString GetJobStateString(const IStructuredJob& job)
{
    TString result;
    {
        TStringOutput output(result);
        job.Save(output);
        output.Finish();
    }
    return result;
}

TVector<TStructuredTablePath> NodeToStructuredTablePaths(const TNode& node)
{
    TVector<TStructuredTablePath> result;
    for (const auto& inputNode : node.AsList()) {
        result.emplace_back(inputNode.AsString());
    }
    return result;
}

TString GetIOInfo(
    const IStructuredJob& job,
    const TString& cluster,
    const TString& transactionId,
    const TString& inputPaths,
    const TString& outputPaths,
    const TString& neededColumns)
{
    auto client = NDetail::CreateClientImpl(cluster);
    TOperationPreparer preparer(client, GetGuid(transactionId));
    auto structuredInputs = NodeToStructuredTablePaths(NodeFromYsonString(inputPaths));
    auto structuredOutputs = NodeToStructuredTablePaths(NodeFromYsonString(outputPaths));

    auto neededColumnsNode = NodeFromYsonString(neededColumns);
    THashSet<TString> columnsUsedInOperations;
    for (const auto& columnNode : neededColumnsNode.AsList()) {
        columnsUsedInOperations.insert(columnNode.AsString());
    }

    auto operationIo = CreateSimpleOperationIoHelper(
        job,
        preparer,
        TOperationOptions(),
        structuredInputs,
        structuredOutputs,
        TUserJobFormatHints(),
        ENodeReaderFormat::Yson,
        columnsUsedInOperations);

    return BuildYsonStringFluently().BeginMap()
        .Item("input_format").Value(operationIo.InputFormat.Config)
        .Item("output_format").Value(operationIo.OutputFormat.Config)
        .Item("input_table_paths").List(operationIo.Inputs)
        .Item("small_files").DoListFor(
            operationIo.JobFiles.begin(),
            operationIo.JobFiles.end(),
            [] (TFluentList fluent, auto fileIt) {
                fluent.Item().BeginMap()
                    .Item("file_name").Value(fileIt->FileName)
                    .Item("data").Value(fileIt->Data)
                .EndMap();
            })
    .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
