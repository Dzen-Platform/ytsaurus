#pragma once

#include "public.h"

#include <ytlib/chunk_client/public.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

ISchemafulWriterPtr CreateSchemafulChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    NChunkClient::IChunkWriterPtr chunkWriter,
    NChunkClient::IBlockCachePtr blockCache);

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
