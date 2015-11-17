#pragma once

#include "public.h"

#include "chunk_reader_base.h"
#include "schemaless_block_reader.h"

#include <ytlib/chunk_client/multi_reader_base.h>

#include <ytlib/node_tracker_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <core/rpc/public.h>

#include <core/concurrency/throughput_throttler.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TRowDescriptor
{
    THorizontalSchemalessBlockReader* BlockReader;
    i32 RowIndex;
};

////////////////////////////////////////////////////////////////////////////////

class TPartitionChunkReader
    : public TChunkReaderBase
{
public:
    TPartitionChunkReader(
        NChunkClient::TSequentialReaderConfigPtr config,
        NChunkClient::IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        NChunkClient::IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const NChunkClient::NProto::TChunkMeta& masterMeta,
        int partitionTag);

    template <class TValueInsertIterator, class TRowDescriptorInsertIterator>
    bool Read(
        TValueInsertIterator& keyValueInserter,
        TRowDescriptorInsertIterator& rowDescriptorInserter,
        i64* rowCount);

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

private:
    const TNameTablePtr NameTable_;
    const TKeyColumns KeyColumns_;

    NChunkClient::NProto::TChunkMeta ChunkMeta_;

    const int PartitionTag_;

    NProto::TBlockMetaExt BlockMetaExt_;
    std::vector<int> IdMapping_;

    int CurrentBlockIndex_ = 0;
    i64 RowCount_ = 0;
    std::vector<std::unique_ptr<THorizontalSchemalessBlockReader>> BlockReaders_;

    THorizontalSchemalessBlockReader* BlockReader_ = nullptr;


    std::vector<NChunkClient::TSequentialReader::TBlockInfo> GetBlockSequence();
    virtual void InitFirstBlock() override;
    virtual void InitNextBlock() override;

    void InitNameTable(TNameTablePtr chunkNameTable);

};

DEFINE_REFCOUNTED_TYPE(TPartitionChunkReader)

////////////////////////////////////////////////////////////////////////////////

class TPartitionMultiChunkReader
    : public NChunkClient::TParallelMultiReaderBase
{
public:
    using TParallelMultiReaderBase::TParallelMultiReaderBase;

    template <class TValueInsertIterator, class TRowDescriptorInsertIterator>
    bool Read(
        TValueInsertIterator& valueInserter,
        TRowDescriptorInsertIterator& rowDescriptorInserter,
        i64* rowCount);

private:
    TPartitionChunkReaderPtr CurrentReader_;

    virtual void OnReaderSwitched() override;

};

DEFINE_REFCOUNTED_TYPE(TPartitionMultiChunkReader)

////////////////////////////////////////////////////////////////////////////////

TPartitionMultiChunkReaderPtr CreatePartitionMultiChunkReader(
    NChunkClient::TMultiChunkReaderConfigPtr config,
    NChunkClient::TMultiChunkReaderOptionsPtr options,
    NApi::IClientPtr client,
    NChunkClient::IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NChunkClient::NProto::TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

#define PARTITION_CHUNK_READER_INL_H_
#include "partition_chunk_reader-inl.h"
#undef PARTITION_CHUNK_READER_INL_H_
