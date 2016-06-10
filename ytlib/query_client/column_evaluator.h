#pragma once

#include "public.h"
#include "evaluation_helpers.h"

#include <yt/ytlib/table_client/unversioned_row.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TColumnEvaluator
    : public TRefCounted
{
public:
    static TColumnEvaluatorPtr Create(
        const TTableSchema& schema,
        const TConstTypeInferrerMapPtr& typeInferrers,
        const TConstFunctionProfilerMapPtr& profilers);

    void EvaluateKey(
        TMutableRow fullRow,
        const TRowBufferPtr& buffer,
        int index) const;

    void EvaluateKeys(
        TMutableRow fullRow,
        const TRowBufferPtr& buffer) const;

    const std::vector<int>& GetReferenceIds(int index) const;
    TConstExpressionPtr GetExpression(int index) const;

    void InitAggregate(
        int schemaId,
        NTableClient::TUnversionedValue* state,
        const TRowBufferPtr& buffer) const;

    void UpdateAggregate(
        int index,
        NTableClient::TUnversionedValue* result,
        const NTableClient::TUnversionedValue& state,
        const NTableClient::TUnversionedValue& update,
        const TRowBufferPtr& buffer) const;

    void MergeAggregate(
        int index,
        NTableClient::TUnversionedValue* result,
        const NTableClient::TUnversionedValue& state,
        const NTableClient::TUnversionedValue& mergeeState,
        const TRowBufferPtr& buffer) const;

    void FinalizeAggregate(
        int index,
        NTableClient::TUnversionedValue* result,
        const NTableClient::TUnversionedValue& state,
        const TRowBufferPtr& buffer) const;

    bool IsAggregate(int index) const;

    size_t GetKeyColumnCount() const;

private:
    struct TColumn
    {
        TCGExpressionCallback Evaluator;
        TCGVariables Variables;
        std::vector<int> ReferenceIds;
        TConstExpressionPtr Expression;
    };

    std::vector<TColumn> Columns_;
    std::unordered_map<int, TCGAggregateCallbacks> Aggregates_;

    TColumnEvaluator(
        std::vector<TColumn> columns,
        std::unordered_map<int, TCGAggregateCallbacks> aggregates);

    DECLARE_NEW_FRIEND();
};

DEFINE_REFCOUNTED_TYPE(TColumnEvaluator);

////////////////////////////////////////////////////////////////////////////////

class TColumnEvaluatorCache
    : public TRefCounted
{
public:
    explicit TColumnEvaluatorCache(
        TColumnEvaluatorCacheConfigPtr config,
        const TConstTypeInferrerMapPtr& typeInferrers = BuiltinTypeInferrersMap,
        const TConstFunctionProfilerMapPtr& profilers = BuiltinFunctionCG);
    ~TColumnEvaluatorCache();

    TColumnEvaluatorPtr Find(const TTableSchema& schema);

private:
    class TImpl;

    DECLARE_NEW_FRIEND();

    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TColumnEvaluatorCache);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

