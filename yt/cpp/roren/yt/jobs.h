#pragma once

#include "yt_io_private.h"

#include <yt/cpp/roren/interface/fwd.h>
#include <yt/cpp/roren/interface/roren.h>
#include <yt/cpp/roren/interface/private/fwd.h>

#include <yt/cpp/mapreduce/interface/client.h>

#include <vector>

namespace NRoren::NPrivate {

////////////////////////////////////////////////////////////////////////////////

NYT::IRawJobPtr CreateParDoMap(
    const IRawParDoPtr& rawParDo,
    const IYtJobInputPtr& input,
    const std::vector<IYtJobOutputPtr>& outputs);

NYT::IRawJobPtr CreateSplitKvMap(
    TRowVtable rowVtable);

NYT::IRawJobPtr CreateSplitKvMap(
    const std::vector<TRowVtable>& rowVtables);

NYT::IRawJobPtr CreateJoinKvReduce(
    const IRawGroupByKeyPtr& rawComputation,
    const TRowVtable& inVtable,
    const IYtJobOutputPtr& output);

NYT::IRawJobPtr CreateMultiJoinKvReduce(
    const IRawCoGroupByKeyPtr& rawComputation,
    const std::vector<TRowVtable>& inVtables,
    const IYtJobOutputPtr& output);

NYT::IRawJobPtr CreateCombineCombiner(
    const IRawCombinePtr& combine,
    const TRowVtable& inRowVtable);

NYT::IRawJobPtr CreateCombineReducer(
    const IRawCombinePtr& combine,
    const TRowVtable& outRowVtable,
    const IYtJobOutputPtr& output);

IExecutionContextPtr CreateYtExecutionContext();

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoren::NPrivate
