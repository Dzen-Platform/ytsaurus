#include "prepared_meta.h"
#include "dispatch_by_type.h"

#include <yt/yt/ytlib/table_client/columnar_chunk_meta.h>

namespace NYT::NNewTableClient {

////////////////////////////////////////////////////////////////////////////////

using TSegmentMetas = TRange<const NProto::TSegmentMeta*>;

void TMetaBase::Init(const NProto::TSegmentMeta& meta)
{
    Offset = meta.offset();
    RowCount = meta.row_count();
    ChunkRowCount = meta.chunk_row_count();
    Type = meta.type();

    if (!IsDense(Type) && meta.HasExtension(NProto::TDenseVersionedSegmentMeta::dense_versioned_segment_meta)) {
        Type = 3;
    }
}

void TTimestampMeta::Init(const NProto::TSegmentMeta& meta)
{
    TMetaBase::Init(meta);

    const auto& timestampMeta = meta.GetExtension(NProto::TTimestampSegmentMeta::timestamp_segment_meta);
    BaseTimestamp = timestampMeta.min_timestamp();
    ExpectedDeletesPerRow = timestampMeta.expected_deletes_per_row();
    ExpectedWritesPerRow = timestampMeta.expected_writes_per_row();
}

void TIntegerMeta::Init(const NProto::TSegmentMeta& meta)
{
    TMetaBase::Init(meta);

    const auto& integerMeta = meta.GetExtension(NProto::TIntegerSegmentMeta::integer_segment_meta);
    BaseValue = integerMeta.min_value();
}

void TBlobMeta::Init(const NProto::TSegmentMeta& meta)
{
    TMetaBase::Init(meta);

    const auto& stringMeta = meta.GetExtension(NProto::TStringSegmentMeta::string_segment_meta);
    ExpectedLength = stringMeta.expected_length();
}

void TDenseMeta::Init(const NProto::TSegmentMeta& meta)
{
    bool dense = meta.HasExtension(NProto::TDenseVersionedSegmentMeta::dense_versioned_segment_meta);
    if (dense) {
        const auto& denseVersionedMeta = meta.GetExtension(NProto::TDenseVersionedSegmentMeta::dense_versioned_segment_meta);
        ExpectedPerRow = denseVersionedMeta.expected_values_per_row();
    }
}

////////////////////////////////////////////////////////////////////////////////

struct TPrepareResult
{
    std::vector<ui32> BlockIds;
    std::vector<ui32> SegmentPivots;
    TSharedRef Meta;
};

template <class TMeta>
static TPrepareResult DoPrepare(TSegmentMetas metas)
{
    auto preparedMeta = TSharedMutableRef::Allocate(sizeof(TMeta) * metas.size());
    auto* preparedMetas = reinterpret_cast<TMeta*>(preparedMeta.begin());

    std::vector<ui32> blockIds;
    std::vector<ui32> segmentPivots;

    // Prepare metas and group by block indexes.
    int lastBlockIndex = -1;
    for (ui32 index = 0; index < metas.size(); ++index) {
        auto blockIndex = metas[index]->block_index();

        if (blockIndex != lastBlockIndex) {
            blockIds.push_back(blockIndex);
            segmentPivots.push_back(index);
            lastBlockIndex = blockIndex;
        }

        preparedMetas[index].Init(*metas[index]);
    }

    segmentPivots.push_back(metas.size());

    return {blockIds, segmentPivots, preparedMeta};
}

struct TColumnInfo
{
    std::vector<ui32> SegmentPivots;
    TSharedRef Meta;

    template <EValueType Type>
    TRange<TKeyMeta<Type>> GetKeyMetas()
    {
        return reinterpret_cast<const TKeyMeta<Type>*>(Meta.begin());
    }

    template <EValueType Type>
    TRange<TValueMeta<Type>> GetValueMetas()
    {
        return reinterpret_cast<const TValueMeta<Type>*>(Meta.begin());
    }

    template <EValueType Type>
    struct TPrepareMeta
    {
        static TPrepareResult Do(TSegmentMetas metas, bool valueColumn)
        {
            if (valueColumn) {
                return DoPrepare<TValueMeta<Type>>(metas);
            } else {
                return DoPrepare<TKeyMeta<Type>>(metas);
            }
        }
    };

    std::vector<ui32> PrepareTimestampMetas(TSegmentMetas metas)
    {
        auto [blockIds, segmentPivots, preparedMeta] = DoPrepare<TTimestampMeta>(metas);

        SegmentPivots = std::move(segmentPivots);
        Meta = std::move(preparedMeta);
        return blockIds;
    }

    std::vector<ui32> PrepareMetas(TSegmentMetas metas, EValueType type, bool versioned)
    {
        auto [blockIds, segmentPivots, preparedMeta] = DispatchByDataType<TPrepareMeta>(type, metas, versioned);

        SegmentPivots = std::move(segmentPivots);
        Meta = std::move(preparedMeta);
        return blockIds;
    }
};

size_t TPreparedChunkMeta::Prepare(
    const NTableClient::TTableSchemaPtr& chunkSchema,
    const NTableClient::TRefCountedColumnMetaPtr& columnMetas)
{
    const auto& chunkSchemaColumns = chunkSchema->Columns();

    THashMap<int, int> firstBlockIdToGroup;

    std::vector<TColumnInfo> preparedColumns;
    // Plus one timestamp column.
    preparedColumns.resize(chunkSchemaColumns.size() + 1);
    GroupIdPerColumn.resize(chunkSchemaColumns.size() + 1);
    ColumnIndexInGroup.resize(chunkSchemaColumns.size() + 1);

    auto determineColumnGroup = [&] (std::vector<ui32> blockIds, int columnIndex) {
        YT_VERIFY(!blockIds.empty());

        auto [it, inserted] = firstBlockIdToGroup.emplace(blockIds.front(), ColumnGroups.size());
        if (inserted) {
            ColumnGroups.emplace_back();
        }

        auto groupId = it->second;
        GroupIdPerColumn[columnIndex] = groupId;

        auto& blockGroup = ColumnGroups[groupId];

        // Fill BlockIds if blockGroup has been created. Otherwise check that BlockIds and blockIds are equal.
        if (inserted) {
            blockGroup.BlockIds = std::move(blockIds);
        } else {
            YT_VERIFY(blockIds == blockGroup.BlockIds);
        }

        ColumnIndexInGroup[columnIndex] = blockGroup.ColumnIds.size();
        blockGroup.ColumnIds.push_back(columnIndex);
    };


    for (int index = 0; index < std::ssize(chunkSchemaColumns); ++index) {
        auto type = GetPhysicalType(chunkSchemaColumns[index].CastToV1Type());
        bool valueColumn = index >= chunkSchema->GetKeyColumnCount();

        auto blockIds = preparedColumns[index].PrepareMetas(
            MakeRange(columnMetas->columns(index).segments()),
            type,
            valueColumn);

        determineColumnGroup(std::move(blockIds), index);
    }

    {
        int timestampReaderIndex = columnMetas->columns().size() - 1;

        auto blockIds = preparedColumns[timestampReaderIndex].PrepareTimestampMetas(
            MakeRange(columnMetas->columns(timestampReaderIndex).segments()));

        determineColumnGroup(std::move(blockIds), timestampReaderIndex);
    }

    std::vector<TRef> blockSegmentMeta;

    for (auto& blockGroup : ColumnGroups) {
        for (int index = 0; index < std::ssize(blockGroup.BlockIds); ++index) {
            for (auto columnId : blockGroup.ColumnIds) {
                auto& [segmentPivots, meta] = preparedColumns[columnId];

                YT_VERIFY(segmentPivots.size() > 0);
                auto segmentCount = segmentPivots.back();
                auto segmentSize = meta.Size() / segmentCount;

                auto offset = segmentPivots[index] * segmentSize;
                auto offsetEnd = segmentPivots[index + 1] * segmentSize;

                blockSegmentMeta.push_back(meta.Slice(offset, offsetEnd));
            }

            auto columnCount = blockSegmentMeta.size();

            size_t size = 0;
            for (const auto& metas : blockSegmentMeta) {
                size += metas.size();
            }

            auto offset = sizeof(ui32) * (columnCount + 1);
            auto mergedMeta = TSharedMutableRef::Allocate(offset + size);

            ui32* offsets = reinterpret_cast<ui32*>(mergedMeta.Begin());
            auto* metasData = reinterpret_cast<char*>(mergedMeta.Begin() + offset);

            for (const auto& metas : blockSegmentMeta) {
                *offsets++ = offset;
                std::copy(metas.begin(), metas.end(), metasData);
                offset += metas.size();
                metasData += metas.size();
            }
            *offsets++ = offset;
            blockGroup.MergedMetas.push_back(mergedMeta);

            blockSegmentMeta.clear();
        }

        YT_VERIFY(blockGroup.MergedMetas.size() == blockGroup.BlockIds.size());
    }

    size_t size = ColumnGroups.capacity() * sizeof(TColumnGroup);
    for (const auto& blockGroup : ColumnGroups) {
        size += blockGroup.BlockIds.capacity() * sizeof(ui32);
        size += blockGroup.ColumnIds.capacity() * sizeof(ui16);
        size += blockGroup.MergedMetas.capacity() * sizeof(TSharedRef);

        for (const auto& perBlockMeta : blockGroup.MergedMetas) {
            size += perBlockMeta.Size();
        }
    }

    return size;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNewTableClient
