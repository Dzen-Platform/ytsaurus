#include "stdafx.h"

#include "partition_chunk_reader.h"

#include "chunk_meta_extensions.h"

#include "name_table.h"
#include "schema.h"
#include "private.h"
#include "helpers.h"

#include <ytlib/chunk_client/config.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/helpers.h>
#include <ytlib/chunk_client/reader_factory.h>

#include <core/concurrency/scheduler.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NRpc;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkReader::TPartitionChunkReader(
    TSequentialReaderConfigPtr config,
    IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TChunkMeta& masterMeta,
    int partitionTag)
    : TChunkReaderBase(
        config,
        underlyingReader,
        GetProtoExtension<TMiscExt>(masterMeta.extensions()),
        blockCache)
    , NameTable_(nameTable)
    , KeyColumns_(keyColumns)
    , ChunkMeta_(masterMeta)
    , PartitionTag_(partitionTag)
{
    ReadyEvent_ = BIND(&TPartitionChunkReader::GetBlockSequence, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run()
        .Apply(BIND(&TPartitionChunkReader::DoOpen, MakeStrong(this)));
}

std::vector<TSequentialReader::TBlockInfo> TPartitionChunkReader::GetBlockSequence()
{
    YCHECK(ChunkMeta_.version() == static_cast<int>(ETableChunkFormat::SchemalessHorizontal));

    std::vector<int> extensionTags = {
        TProtoExtensionTag<TBlockMetaExt>::Value,
        TProtoExtensionTag<TNameTableExt>::Value,
        TProtoExtensionTag<TKeyColumnsExt>::Value
    };

    auto errorOrMeta = WaitFor(UnderlyingReader_->GetMeta(PartitionTag_, extensionTags));
    THROW_ERROR_EXCEPTION_IF_FAILED(errorOrMeta);

    ChunkMeta_ = errorOrMeta.Value();

    TNameTablePtr chunkNameTable;
    auto nameTableExt = GetProtoExtension<TNameTableExt>(ChunkMeta_.extensions());
    FromProto(&chunkNameTable, nameTableExt);

    InitNameTable(chunkNameTable);

    auto keyColumnsExt = GetProtoExtension<TKeyColumnsExt>(ChunkMeta_.extensions());
    auto chunkKeyColumns = NYT::FromProto<TKeyColumns>(keyColumnsExt);
    YCHECK(chunkKeyColumns == KeyColumns_);

    BlockMetaExt_ = GetProtoExtension<TBlockMetaExt>(ChunkMeta_.extensions());
    std::vector<TSequentialReader::TBlockInfo> blocks;
    for (auto& blockMeta : BlockMetaExt_.blocks()) {
        TSequentialReader::TBlockInfo blockInfo;
        blockInfo.Index = blockMeta.block_index();
        blockInfo.UncompressedDataSize = blockMeta.uncompressed_size();
        blocks.push_back(blockInfo);
    }

    return blocks;
}

TDataStatistics TPartitionChunkReader::GetDataStatistics() const
{
    auto dataStatistics = TChunkReaderBase::GetDataStatistics();
    dataStatistics.set_row_count(RowCount_);
    return dataStatistics;
}

void TPartitionChunkReader::InitFirstBlock()
{
    BlockReader_ = new THorizontalSchemalessBlockReader(
            SequentialReader_->GetCurrentBlock(),
            BlockMetaExt_.blocks(CurrentBlockIndex_),
            IdMapping_,
            KeyColumns_.size());

    BlockReaders_.emplace_back(BlockReader_);
}

void TPartitionChunkReader::InitNextBlock()
{
    ++CurrentBlockIndex_;
    InitFirstBlock();
}

void TPartitionChunkReader::InitNameTable(TNameTablePtr chunkNameTable)
{
    IdMapping_.resize(chunkNameTable->GetSize());

    for (int chunkNameId = 0; chunkNameId < chunkNameTable->GetSize(); ++chunkNameId) {
        auto name = chunkNameTable->GetName(chunkNameId);
        auto id = NameTable_->GetIdOrRegisterName(name);
        IdMapping_[chunkNameId] = id;
    }
}

////////////////////////////////////////////////////////////////////////////////

void TPartitionMultiChunkReader::OnReaderSwitched()
{
    CurrentReader_ = dynamic_cast<TPartitionChunkReader*>(CurrentSession_.Reader.Get());
    YCHECK(CurrentReader_);
}

TPartitionMultiChunkReaderPtr CreatePartitionMultiChunkReader(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    IClientPtr client,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns)
{
    std::vector<IReaderFactoryPtr> factories;
    for (const auto& chunkSpec : chunkSpecs) {
        auto memoryEstimate = GetChunkReaderMemoryEstimate(chunkSpec, config);
        auto createReader = [=] () {
            auto remoteReader = CreateRemoteReader(
                chunkSpec,
                config,
                options,
                client,
                nodeDirectory,
                blockCache,
                GetUnlimitedThrottler());

            YCHECK(!chunkSpec.has_channel());
            YCHECK(!chunkSpec.has_lower_limit());
            YCHECK(!chunkSpec.has_upper_limit());
            YCHECK(chunkSpec.has_partition_tag());

            TSequentialReaderConfigPtr sequentialReaderConfig = config;

            return New<TPartitionChunkReader>(
                sequentialReaderConfig,
                remoteReader,
                nameTable,
                blockCache,
                keyColumns,
                chunkSpec.chunk_meta(),
                chunkSpec.partition_tag());
        };

        factories.emplace_back(CreateReaderFactory(
            createReader, 
            memoryEstimate));
    }

    return New<TPartitionMultiChunkReader>(
        config, 
        options, 
        factories);
};

////////////////////////////////////////////////////////////////////////////////


} // namespace NTableClient
} // namespace NYT

