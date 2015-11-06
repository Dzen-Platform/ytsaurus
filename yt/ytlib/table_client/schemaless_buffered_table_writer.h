#pragma once

#include "public.h"

#include <ytlib/api/public.h>

#include <ytlib/api/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/ypath/public.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

ISchemalessWriterPtr CreateSchemalessBufferedTableWriter(
    TBufferedTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    NApi::IClientPtr client,
    TNameTablePtr nameTable,
    const NYPath::TYPath& path);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
