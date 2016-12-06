#pragma once

#include "public.h"

#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/misc/workload.h>

namespace NYT {
namespace NQueryClient {

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

struct TDataRange
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    TRowRange Range;
};

struct TDataRanges
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    //! Mount revision for a tablet.
    i64 MountRevision;
    TSharedRange<TRowRange> Ranges;
    //! If |true|, these ranges could be reclassified into a set of discrete lookup keys.
    bool LookupSupported = true;
};

struct TDataKeys
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    TSharedRange<TRow> Keys;
};

struct TQueryOptions
{
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::SyncLastCommittedTimestamp;
    bool VerboseLogging = false;
    int MaxSubqueries = std::numeric_limits<int>::max();
    ui64 RangeExpansionLimit = 0;
    bool EnableCodeCache = true;
    TWorkloadDescriptor WorkloadDescriptor;
};

struct TBaseColumn
{
    TBaseColumn(const Stroka& name, EValueType type)
        : Name(name)
        , Type(type)
    { }

    Stroka Name;
    EValueType Type;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
