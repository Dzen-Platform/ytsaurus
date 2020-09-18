#pragma once

#include "private.h"

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/client/ypath/public.h>

#include <yt/core/logging/public.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

struct TTable
    : public TRefCounted
    , public NChunkClient::TUserObject
{
    NTableClient::TTableSchemaPtr Schema;
    //! Operand index according to JOIN clause (if any):
    //! - SELECT * FROM AAA: AAA.TableIndex = 0.
    //! - SELECT * FROM AAA JOIN BBB: AAA.TableIndex = 0, BBB.TableIndex = 1.
    //! If operand consists of several tables (like in concat* case), all of them share
    //! same operand index.
    //! NB: Currently, CH handles multi-JOIN as a left-associative sequence of two-operand joins.
    //! In particular,
    //! - SELECT * FROM AAA JOIN BBB JOIN CCC is actually (SELECT * FROM AAA JOIN BBB) JOIN CCC.
    //! Thus, OperandIndex is always 0 or 1.
    int OperandIndex = 0;
    bool Dynamic = false;
    bool IsPartitioned = false;

    TTable(NYPath::TRichYPath path, const NYTree::IAttributeDictionaryPtr& attributes);
};

////////////////////////////////////////////////////////////////////////////////

// Fetches tables for given paths.
// If `skipUnsuitableNodes` is set, skips all non-static-table items,
// otherwise throws an error for them.
std::vector<TTablePtr> FetchTables(
    const NApi::NNative::IClientPtr& client,
    THost* host,
    const std::vector<NYPath::TRichYPath>& richPaths,
    bool skipUnsuitableNodes,
    NLogging::TLogger logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
