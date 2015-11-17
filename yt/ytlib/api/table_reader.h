#include "client.h"

#include <ytlib/table_client/public.h>

#include <ytlib/ypath/public.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

TFuture<NTableClient::ISchemalessMultiChunkReaderPtr> CreateTableReader(
    IClientPtr client,
    const NYPath::TRichYPath& path,
    const TTableReaderOptions& options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
