#pragma once

#include "public.h"
#include "callbacks.h"
#include "function_context.h"

#include <yt/ytlib/api/rowset.h>

#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/codegen/function.h>

#include <yt/core/misc/chunked_memory_pool.h>

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include <sparsehash/dense_hash_set>
#include <sparsehash/dense_hash_map>

namespace NYT {
namespace NQueryClient {

const i64 PoolChunkSize = 64 * 1024;
const double MaxSmallBlockRatio = 1.0;
const size_t RowsetProcessingSize = 1024;
const size_t WriteRowsetSize = 64 * RowsetProcessingSize;

////////////////////////////////////////////////////////////////////////////////

class TInterruptedCompleteException
{ };

class TInterruptedIncompleteException
{ };

struct TOutputBufferTag
{ };

struct TIntermadiateBufferTag
{ };

struct TPermanentBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

static const size_t InitialGroupOpHashtableCapacity = 1024;

using THasherFunction = ui64(const TValue*);
using TComparerFunction = char(const TValue*, const TValue*);
using TTernaryComparerFunction = i64(const TValue*, const TValue*);

namespace NDetail {
class TGroupHasher
{
public:
    TGroupHasher(THasherFunction* ptr)
        : Ptr_(ptr)
    { }

    ui64 operator () (const TValue* row) const
    {
        return Ptr_(row);
    }

private:
    THasherFunction* Ptr_;
};

class TRowComparer
{
public:
    TRowComparer(TComparerFunction* ptr)
        : Ptr_(ptr)
    { }

    bool operator () (const TValue* a, const TValue* b) const
    {
        return a == b || a && b && Ptr_(a, b);
    }

private:
    TComparerFunction* Ptr_;
};
} // namespace NDetail

using TLookupRows = google::sparsehash::dense_hash_set<
    const TValue*,
    NDetail::TGroupHasher,
    NDetail::TRowComparer>;

using TJoinLookup = google::sparsehash::dense_hash_map<
    const TValue*,
    std::pair<int, bool>,
    NDetail::TGroupHasher,
    NDetail::TRowComparer>;

using TJoinLookupRows = std::unordered_multiset<
    const TValue*,
    NDetail::TGroupHasher,
    NDetail::TRowComparer>;

struct TExecutionContext;

struct TJoinParameters
{
    bool IsOrdered;
    bool IsLeft;
    bool IsSortMergeJoin;
    bool IsPartiallySorted;
    std::vector<size_t> SelfColumns;
    std::vector<size_t> ForeignColumns;
    TJoinSubqueryEvaluator ExecuteForeign;
    size_t BatchSize;
    size_t CommonKeyPrefixDebug;
    size_t PrimaryRowSize;
};

struct TSingleJoinParameters
{
    size_t KeySize;
    bool IsLeft;
    bool IsPartiallySorted;
    std::vector<size_t> ForeignColumns;
    TJoinSubqueryEvaluator ExecuteForeign;
};

struct TMultiJoinParameters
{
    SmallVector<TSingleJoinParameters, 10> Items;
    size_t PrimaryRowSize;
    size_t BatchSize;
};

struct TChainedRow
{
    const TValue* Row;
    const TValue* Key;
    int NextRowIndex;
};

struct TJoinClosure
{
    TRowBufferPtr Buffer;
    TJoinLookup Lookup;
    std::vector<TChainedRow> ChainedRows;

    TComparerFunction* PrefixEqComparer;
    int KeySize;

    const TValue* LastKey = nullptr;
    std::vector<std::pair<const TValue*, int>> KeysToRows;
    size_t CommonKeyPrefixDebug;
    size_t PrimaryRowSize;

    size_t BatchSize;
    std::function<void()> ProcessJoinBatch;
    std::function<void()> ProcessSegment;

    TJoinClosure(
        THasherFunction* lookupHasher,
        TComparerFunction* lookupEqComparer,
        TComparerFunction* prefixEqComparer,
        int keySize,
        int primaryRowSize,
        size_t batchSize);
};

struct TMultiJoinClosure
{
    TRowBufferPtr Buffer;

    typedef google::sparsehash::dense_hash_set<
        TValue*,
        NDetail::TGroupHasher,
        NDetail::TRowComparer> THashJoinLookup;  // + slot after row

    std::vector<TValue*> PrimaryRows;

    struct TItem
    {
        TRowBufferPtr Buffer;
        size_t KeySize;
        TComparerFunction* PrefixEqComparer;

        THashJoinLookup Lookup;
        std::vector<TValue*> OrderedKeys;  // + slot after row
        TValue* LastKey = nullptr;

        TItem(
            size_t keySize,
            TComparerFunction* prefixEqComparer,
            THasherFunction* lookupHasher,
            TComparerFunction* lookupEqComparer);
    };

    SmallVector<TItem, 32> Items;

    size_t PrimaryRowSize;
    size_t BatchSize;
    std::function<void(size_t)> ProcessSegment;
    std::function<void()> ProcessJoinBatch;
};

struct TGroupByClosure
{
    TRowBufferPtr Buffer;
    TLookupRows Lookup;
    std::vector<const TValue*> GroupedRows;
    int KeySize;
    bool CheckNulls;

    TGroupByClosure(
        THasherFunction* groupHasher,
        TComparerFunction* groupComparer,
        int keySize,
        bool checkNulls);
};

struct TWriteOpClosure
{
    TRowBufferPtr OutputBuffer;

    // Rows stored in OutputBuffer
    std::vector<TRow> OutputRowsBatch;
    size_t RowSize;

    TWriteOpClosure();

};

typedef TRowBuffer TExpressionContext;

#define CHECK_STACK() (void) 0;

struct TExecutionContext
{
    ISchemafulReaderPtr Reader;
    ISchemafulWriterPtr Writer;

    TQueryStatistics* Statistics;

    // These limits prevent full scan.
    i64 InputRowLimit;
    i64 OutputRowLimit;
    i64 GroupRowLimit;
    i64 JoinRowLimit;

    // Limit from LIMIT clause.
    i64 Limit;

    TExecutionContext()
    {
        auto context = this;
        Y_UNUSED(context);
        CHECK_STACK();
    }
    bool IsOrdered = false;

};

class TTopCollector
{
    class TComparer
    {
    public:
        explicit TComparer(TComparerFunction* ptr)
            : Ptr_(ptr)
        { }

        bool operator() (const std::pair<const TValue*, int>& lhs, const std::pair<const TValue*, int>& rhs) const
        {
            return (*this)(lhs.first, rhs.first);
        }

        bool operator () (const TValue* a, const TValue* b) const
        {
            return Ptr_(a, b);
        }

    private:
        TComparerFunction* const Ptr_;
    };

public:
    TTopCollector(i64 limit, TComparerFunction* comparer, size_t rowSize);

    std::vector<const TValue*> GetRows() const;

    void AddRow(const TValue* row);

private:
    // GarbageMemorySize <= AllocatedMemorySize <= TotalMemorySize
    size_t TotalMemorySize_ = 0;
    size_t AllocatedMemorySize_ = 0;
    size_t GarbageMemorySize_ = 0;

    TComparer Comparer_;
    size_t RowSize_;

    std::vector<TRowBufferPtr> Buffers_;
    std::vector<int> EmptyBufferIds_;
    std::vector<std::pair<const TValue*, int>> Rows_;

    std::pair<const TValue*, int> Capture(const TValue* row);

    void AccountGarbage(const TValue* row);

};

class TCGVariables
{
public:
    template <class T, class... Args>
    size_t AddOpaque(Args&&... args)
    {
        auto pointer = new T(std::forward<Args>(args)...);
        auto deleter = [] (void* ptr) {
            static_assert(sizeof(T) > 0, "Cannot delete incomplete type.");
            delete static_cast<T*>(ptr);
        };

        std::unique_ptr<void, void(*)(void*)> holder(pointer, deleter);
        YCHECK(holder);

        OpaqueValues_.push_back(std::move(holder));
        OpaquePointers_.push_back(pointer);

        return OpaquePointers_.size() - 1;
    }

    void* const* GetOpaqueData() const
    {
        return OpaquePointers_.data();
    }

    void Clear()
    {
        OpaquePointers_.clear();
        OpaqueValues_.clear();
    }

    TValue* GetLiteralvalues()
    {
        LiteralsRow = std::make_unique<TValue[]>(LiteralValues.size());
        size_t index = 0;
        for (const auto& value : LiteralValues) {
            LiteralsRow[index++] = TValue(value);
        }
        return LiteralsRow.get();
    }

    std::unique_ptr<TValue[]> LiteralsRow;
    std::vector<TOwningValue> LiteralValues;

private:
    std::vector<std::unique_ptr<void, void(*)(void*)>> OpaqueValues_;
    std::vector<void*> OpaquePointers_;

};

typedef void (TCGQuerySignature)(const TValue*, void* const*, TExecutionContext*);
typedef void (TCGExpressionSignature)(const TValue*, void* const*, TValue*, const TValue*, TExpressionContext*);
typedef void (TCGAggregateInitSignature)(TExpressionContext*, TValue*);
typedef void (TCGAggregateUpdateSignature)(TExpressionContext*, TValue*, const TValue*);
typedef void (TCGAggregateMergeSignature)(TExpressionContext*, TValue*, const TValue*);
typedef void (TCGAggregateFinalizeSignature)(TExpressionContext*, TValue*, const TValue*);

using TCGQueryCallback = NCodegen::TCGFunction<TCGQuerySignature>;
using TCGExpressionCallback = NCodegen::TCGFunction<TCGExpressionSignature>;
using TCGAggregateInitCallback = NCodegen::TCGFunction<TCGAggregateInitSignature>;
using TCGAggregateUpdateCallback = NCodegen::TCGFunction<TCGAggregateUpdateSignature>;
using TCGAggregateMergeCallback = NCodegen::TCGFunction<TCGAggregateMergeSignature>;
using TCGAggregateFinalizeCallback = NCodegen::TCGFunction<TCGAggregateFinalizeSignature>;

struct TCGAggregateCallbacks
{
    TCGAggregateInitCallback Init;
    TCGAggregateUpdateCallback Update;
    TCGAggregateMergeCallback Merge;
    TCGAggregateFinalizeCallback Finalize;
};

////////////////////////////////////////////////////////////////////////////////

std::pair<TQueryPtr, TDataRanges> GetForeignQuery(
    TQueryPtr subquery,
    TConstJoinClausePtr joinClause,
    std::vector<TRow> keys,
    TRowBufferPtr permanentBuffer);

////////////////////////////////////////////////////////////////////////////////

struct TExpressionClosure;

struct TJoinComparers
{
    TComparerFunction* PrefixEqComparer;
    THasherFunction* SuffixHasher;
    TComparerFunction* SuffixEqComparer;
    TComparerFunction* SuffixLessComparer;
    TComparerFunction* ForeignPrefixEqComparer;
    TComparerFunction* ForeignSuffixLessComparer;
    TTernaryComparerFunction* FullTernaryComparer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
