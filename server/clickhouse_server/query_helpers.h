#pragma once

#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

DB::ASTTableExpression* GetFirstTableExpression(DB::ASTSelectQuery& select);

std::vector<DB::ASTTableExpression*> GetAllTableExpressions(DB::ASTSelectQuery& select);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
