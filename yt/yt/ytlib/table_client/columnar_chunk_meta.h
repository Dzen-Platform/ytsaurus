#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"

#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/yt/client/table_client/schema.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

using TRefCountedColumnMeta = TRefCountedProto<NProto::TColumnMetaExt>;
using TRefCountedColumnMetaPtr = TIntrusivePtr<TRefCountedColumnMeta>;

////////////////////////////////////////////////////////////////////////////////

class TColumnarChunkMeta
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NChunkClient::EChunkType, ChunkType);
    DEFINE_BYVAL_RO_PROPERTY(NChunkClient::EChunkFormat, ChunkFormat);
    DEFINE_BYREF_RO_PROPERTY(TRefCountedBlockMetaPtr, BlockMeta);
    DEFINE_BYREF_RO_PROPERTY(TRefCountedColumnMetaPtr, ColumnMeta);
    DEFINE_BYREF_RO_PROPERTY(NChunkClient::NProto::TMiscExt, Misc);
    DEFINE_BYREF_RO_PROPERTY(TSharedRange<TLegacyKey>, LegacyBlockLastKeys);
    DEFINE_BYREF_RO_PROPERTY(TSharedRange<TKey>, BlockLastKeys);
    DEFINE_BYVAL_RO_PROPERTY(TTableSchemaPtr, ChunkSchema);
    DEFINE_BYREF_RO_PROPERTY(TNameTablePtr, ChunkNameTable);

public:
    explicit TColumnarChunkMeta(const NChunkClient::NProto::TChunkMeta& chunkMeta);

    void InitBlockLastKeys(const TKeyColumns& keyColumns);
    void RenameColumns(const TColumnRenameDescriptors& renameDescriptros);

    virtual i64 GetMemoryUsage() const;

protected:
    TColumnarChunkMeta() = default;
    void InitExtensions(const NChunkClient::NProto::TChunkMeta& chunkMeta);

private:
    i64 BlockLastKeysSize_;
};

DEFINE_REFCOUNTED_TYPE(TColumnarChunkMeta)

////////////////////////////////////////////////////////////////////////////////

TTableSchemaPtr GetTableSchema(const NChunkClient::NProto::TChunkMeta& chunkMeta);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
