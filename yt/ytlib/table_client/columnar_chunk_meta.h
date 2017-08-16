#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

using TRefCountedBlockMeta = TRefCountedProto<NProto::TBlockMetaExt>;
using TRefCountedBlockMetaPtr = TIntrusivePtr<TRefCountedBlockMeta>;

using TRefCountedColumnMeta = TRefCountedProto<NProto::TColumnMetaExt>;
using TRefCountedColumnMetaPtr = TIntrusivePtr<TRefCountedColumnMeta>;

class TColumnarChunkMeta
    : public TIntrinsicRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NChunkClient::EChunkType, ChunkType);
    DEFINE_BYVAL_RO_PROPERTY(NTableClient::ETableChunkFormat, ChunkFormat);
    DEFINE_BYREF_RO_PROPERTY(TRefCountedBlockMetaPtr, BlockMeta);
    DEFINE_BYREF_RO_PROPERTY(TRefCountedColumnMetaPtr, ColumnMeta);
    DEFINE_BYREF_RO_PROPERTY(NChunkClient::NProto::TMiscExt, Misc);
    DEFINE_BYREF_RO_PROPERTY(TSharedRange<TKey>, BlockLastKeys);
    DEFINE_BYREF_RO_PROPERTY(TTableSchema, ChunkSchema);

public:
    explicit TColumnarChunkMeta(const NChunkClient::NProto::TChunkMeta& chunkMeta);
    void InitBlockLastKeys(const TKeyColumns& keyColumns);

protected:
    TColumnarChunkMeta() = default;
    void InitExtensions(const NChunkClient::NProto::TChunkMeta& chunkMeta);

};

DEFINE_REFCOUNTED_TYPE(TColumnarChunkMeta)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
