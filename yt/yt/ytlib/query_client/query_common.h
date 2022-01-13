#pragma once

#include "public.h"

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/ytlib/tablet_client/helpers.h>

#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/client/misc/workload.h>

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

typedef std::pair<int, int> TSourceLocation;
static const TSourceLocation NullSourceLocation(0, 0);

DEFINE_ENUM(EUnaryOp,
    // Arithmetical operations.
    (Plus)
    (Minus)
    // Integral operations.
    (BitNot)
    // Logical operations.
    (Not)
);

DEFINE_ENUM(EBinaryOp,
    // Arithmetical operations.
    (Plus)
    (Minus)
    (Multiply)
    (Divide)
    // Integral operations.
    (Modulo)
    (LeftShift)
    (RightShift)
    (BitOr)
    (BitAnd)
    // Logical operations.
    (And)
    (Or)
    // Relational operations.
    (Equal)
    (NotEqual)
    (Less)
    (LessOrEqual)
    (Greater)
    (GreaterOrEqual)
);

DEFINE_ENUM(ETotalsMode,
    (None)
    (BeforeHaving)
    (AfterHaving)
);

DEFINE_ENUM(EAggregateFunction,
    (Sum)
    (Min)
    (Max)
);

const char* GetUnaryOpcodeLexeme(EUnaryOp opcode);
const char* GetBinaryOpcodeLexeme(EBinaryOp opcode);

//! Reverse binary opcode for comparison operations (for swapping arguments).
EBinaryOp GetReversedBinaryOpcode(EBinaryOp opcode);

//! Inverse binary opcode for comparison operations (for inverting the operation).
EBinaryOp GetInversedBinaryOpcode(EBinaryOp opcode);

//! Classifies binary opcode according to classification above.
bool IsArithmeticalBinaryOp(EBinaryOp opcode);

//! Classifies binary opcode according to classification above.
bool IsIntegralBinaryOp(EBinaryOp opcode);

//! Classifies binary opcode according to classification above.
bool IsLogicalBinaryOp(EBinaryOp opcode);

//! Classifies binary opcode according to classification above.
bool IsRelationalBinaryOp(EBinaryOp opcode);

////////////////////////////////////////////////////////////////////////////////

struct TDataSource
{
    // Could be:
    // * a table id;
    // * a tablet id.
    NObjectClient::TObjectId ObjectId;
    // If #ObjectId is a tablet id then this is the id of the cell hosting this tablet.
    // COMPAT(babenko): legacy clients may omit this field.
    NObjectClient::TCellId CellId;

    NHydra::TRevision MountRevision;

    TSharedRange<TRowRange> Ranges;
    std::vector<NTableClient::TLogicalTypePtr> Schema;
    TSharedRange<TRow> Keys;

    //! If |true|, these ranges could be reclassified into a set of discrete lookup keys.
    bool LookupSupported = true;

    size_t KeyWidth = 0;
};

struct TQueryBaseOptions
{
    i64 InputRowLimit = std::numeric_limits<i64>::max();
    i64 OutputRowLimit = std::numeric_limits<i64>::max();

    bool EnableCodeCache = true;
    bool UseMultijoin = true;
    NChunkClient::TReadSessionId ReadSessionId;
    size_t MemoryLimitPerNode = std::numeric_limits<size_t>::max();
};

struct TQueryOptions
    : public TQueryBaseOptions
{
    NTabletClient::TReadTimestampRange TimestampRange{
        .Timestamp = NTransactionClient::SyncLastCommittedTimestamp,
        .RetentionTimestamp = NTransactionClient::NullTimestamp,
    };
    bool VerboseLogging = false;
    int MaxSubqueries = std::numeric_limits<int>::max();
    ui64 RangeExpansionLimit = 0;
    TWorkloadDescriptor WorkloadDescriptor;
    bool AllowFullScan = true;
    TInstant Deadline = TInstant::Max();
    bool SuppressAccessTracking = false;
    std::optional<TString> ExecutionPool;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
