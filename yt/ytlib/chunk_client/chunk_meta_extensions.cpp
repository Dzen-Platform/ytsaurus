﻿#include "stdafx.h"
#include "chunk_meta_extensions.h"

#include <ytlib/table_client/chunk_meta_extensions.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NTableClient::NProto;

///////////////////////////////////////////////////////////////////////////////

TChunkMeta FilterChunkMetaByExtensionTags(
    const TChunkMeta& chunkMeta,
    const TNullable<std::vector<int>>& extensionTags)
{
    if (!extensionTags) {
        return chunkMeta;
    }

    TChunkMeta filteredChunkMeta;
    filteredChunkMeta.set_type(chunkMeta.type());
    filteredChunkMeta.set_version(chunkMeta.version());

    FilterProtoExtensions(
        filteredChunkMeta.mutable_extensions(),
        chunkMeta.extensions(),
        yhash_set<int>(extensionTags->begin(), extensionTags->end()));

    return filteredChunkMeta;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
