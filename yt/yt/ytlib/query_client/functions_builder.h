#pragma once

#include "public.h"

#include "functions_common.h"

#include <yt/yt/core/misc/ref.h>

#define UDF_BC(name) TSharedRef::FromString(::NResource::Find(TString("/llvm_bc/") + #name))

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

struct TFunctionRegistryBuilder
{
    TFunctionRegistryBuilder(
        const TTypeInferrerMapPtr& typeInferrers,
        const TFunctionProfilerMapPtr& functionProfilers,
        const TAggregateProfilerMapPtr& aggregateProfilers)
        : TypeInferrers_(typeInferrers)
        , FunctionProfilers_(functionProfilers)
        , AggregateProfilers_(aggregateProfilers)
    { }

    TTypeInferrerMapPtr TypeInferrers_;
    TFunctionProfilerMapPtr FunctionProfilers_;
    TAggregateProfilerMapPtr AggregateProfilers_;

    void RegisterFunction(
        const TString& functionName,
        const TString& symbolName,
        std::unordered_map<TTypeArgument, TUnionType> typeArgumentConstraints,
        std::vector<TType> argumentTypes,
        TType repeatedArgType,
        TType resultType,
        TSharedRef implementationFile,
        ICallingConventionPtr callingConvention,
        bool useFunctionContext = false);

    void RegisterFunction(
        const TString& functionName,
        std::vector<TType> argumentTypes,
        TType resultType,
        TSharedRef implementationFile,
        ECallingConvention callingConvention);

    void RegisterFunction(
        const TString& functionName,
        std::unordered_map<TTypeArgument, TUnionType> typeArgumentConstraints,
        std::vector<TType> argumentTypes,
        TType repeatedArgType,
        TType resultType,
        TSharedRef implementationFile);

    void RegisterAggregate(
        const TString& aggregateName,
        std::unordered_map<TTypeArgument, TUnionType> typeArgumentConstraints,
        TType argumentType,
        TType resultType,
        TType stateType,
        TSharedRef implementationFile,
        ECallingConvention callingConvention,
        bool isFirst = false);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
