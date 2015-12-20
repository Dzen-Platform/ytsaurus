#include "read_limit.h"

#include <yt/core/misc/format.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node.h>

namespace NYT {
namespace NChunkClient {

using namespace NYTree;
using namespace NYson;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TReadLimit::TReadLimit()
{ }

TReadLimit::TReadLimit(const NProto::TReadLimit& protoLimit)
{
    InitCopy(protoLimit);
}

TReadLimit::TReadLimit(NProto::TReadLimit&& protoLimit)
{
    InitMove(std::move(protoLimit));
}

TReadLimit::TReadLimit(const TOwningKey& key)
{
    SetKey(key);
}

TReadLimit::TReadLimit(TOwningKey&& key)
{
    SetKey(std::move(key));
}

TReadLimit& TReadLimit::operator= (const NProto::TReadLimit& protoLimit)
{
    InitCopy(protoLimit);
    return *this;
}

TReadLimit& TReadLimit::operator= (NProto::TReadLimit&& protoLimit)
{
    InitMove(std::move(protoLimit));
    return *this;
}

TReadLimit TReadLimit::GetSuccessor() const
{
    TReadLimit result;
    if (HasKey()) {
        auto key = GetKey();
        result.SetKey(GetKeyPrefixSuccessor(key.Get(), key.GetCount()));
    }
    if (HasRowIndex()) {
        result.SetRowIndex(GetRowIndex() + 1);
    }
    if (HasChunkIndex()) {
        result.SetChunkIndex(GetChunkIndex() + 1);
    }
    return result;
}

const NProto::TReadLimit& TReadLimit::AsProto() const
{
    return ReadLimit_;
}

const TOwningKey& TReadLimit::GetKey() const
{
    YASSERT(HasKey());
    return Key_;
}

bool TReadLimit::HasKey() const
{
    return ReadLimit_.has_key();
}

void TReadLimit::SetKey(const TOwningKey& key)
{
    Key_ = key;
    ToProto(ReadLimit_.mutable_key(), Key_);
}

void TReadLimit::SetKey(TOwningKey&& key)
{
    swap(Key_, key);
    ToProto(ReadLimit_.mutable_key(), Key_);
}

i64 TReadLimit::GetRowIndex() const
{
    YASSERT(HasRowIndex());
    return ReadLimit_.row_index();
}

bool TReadLimit::HasRowIndex() const
{
    return ReadLimit_.has_row_index();
}

void TReadLimit::SetRowIndex(i64 rowIndex)
{
    ReadLimit_.set_row_index(rowIndex);
}

i64 TReadLimit::GetOffset() const
{
    YASSERT(HasOffset());
    return ReadLimit_.offset();
}

bool TReadLimit::HasOffset() const
{
    return ReadLimit_.has_offset();
}

void TReadLimit::SetOffset(i64 offset)
{
    ReadLimit_.set_offset(offset);
}

i64 TReadLimit::GetChunkIndex() const
{
    YASSERT(HasChunkIndex());
    return ReadLimit_.chunk_index();
}

bool TReadLimit::HasChunkIndex() const
{
    return ReadLimit_.has_chunk_index();
}

void TReadLimit::SetChunkIndex(i64 chunkIndex)
{
    ReadLimit_.set_chunk_index(chunkIndex);
}

bool TReadLimit::IsTrivial() const
{
    return NChunkClient::IsTrivial(ReadLimit_);
}

void TReadLimit::Persist(NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, ReadLimit_);
    Persist(context, Key_);
}

void TReadLimit::MergeLowerKey(const TOwningKey& key)
{
    if (!HasKey() || GetKey() < key) {
        SetKey(key);
    }
}

void TReadLimit::MergeUpperKey(const TOwningKey& key)
{
    if (!HasKey() || GetKey() > key) {
        SetKey(key);
    }
}

void TReadLimit::MergeLowerRowIndex(i64 rowIndex)
{
    if (!HasRowIndex() || GetRowIndex() < rowIndex) {
        SetRowIndex(rowIndex);
    }   
}

void TReadLimit::MergeUpperRowIndex(i64 rowIndex)
{
    if (!HasRowIndex() || GetRowIndex() > rowIndex) {
        SetRowIndex(rowIndex);
    }   
}

void TReadLimit::MergeLowerLimit(const NProto::TReadLimit& readLimit)
{
    if (readLimit.has_row_index()) {
        MergeLowerRowIndex(readLimit.row_index());
    }
    if (readLimit.has_chunk_index() && (!HasChunkIndex() || GetChunkIndex() < readLimit.chunk_index())) {
        SetChunkIndex(readLimit.chunk_index());
    }
    if (readLimit.has_offset() && (!HasOffset() || GetOffset() < readLimit.offset())) {
        SetOffset(readLimit.offset());
    }
    if (readLimit.has_key()) {
        auto key = NYT::FromProto<TOwningKey>(readLimit.key());
        MergeLowerKey(key);
    }
}

void TReadLimit::MergeUpperLimit(const NProto::TReadLimit& readLimit)
{
    if (readLimit.has_row_index()) {
        MergeUpperRowIndex(readLimit.row_index());
    }
    if (readLimit.has_chunk_index() && (!HasChunkIndex() || GetChunkIndex() > readLimit.chunk_index())) {
        SetChunkIndex(readLimit.chunk_index());
    }
    if (readLimit.has_offset() && (!HasOffset() || GetOffset() > readLimit.offset())) {
        SetOffset(readLimit.offset());
    }
    if (readLimit.has_key()) {
        auto key = NYT::FromProto<TOwningKey>(readLimit.key());
        MergeUpperKey(key);
    }
}

void TReadLimit::InitKey()
{
    if (ReadLimit_.has_key()) {
        FromProto(&Key_, ReadLimit_.key());
    }
}

void TReadLimit::InitCopy(const NProto::TReadLimit& readLimit)
{
    ReadLimit_.CopyFrom(readLimit);
    InitKey();
}

void TReadLimit::InitMove(NProto::TReadLimit&& readLimit)
{
    ReadLimit_.Swap(&readLimit);
    InitKey();
}

size_t TReadLimit::SpaceUsed() const
{
    return
       sizeof(*this) +
       ReadLimit_.SpaceUsed() - sizeof(ReadLimit_) +
       Key_.GetSpaceUsed() - sizeof(Key_);
}

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TReadLimit& limit)
{
    using ::ToString;

    TStringBuilder builder;
    auto append = [&] (const char* label, const TStringBuf& value) {
        if (builder.GetLength() > 0) {
            builder.AppendString(", ");
        }
        builder.AppendString(label);
        builder.AppendString(": ");
        builder.AppendString(value);
    };

    if (limit.HasKey()) {
        append("Key", ToString(limit.GetKey()));
    }

    if (limit.HasRowIndex()) {
        append("RowIndex", ToString(limit.GetRowIndex()));
    }

    if (limit.HasOffset()) {
        append("Offset", ToString(limit.GetOffset()));
    }

    if (limit.HasChunkIndex()) {
        append("ChunkIndex", ToString(limit.GetOffset()));
    }

    return builder.Flush();
}

bool IsNontrivial(const TReadLimit& limit)
{
    return !IsTrivial(limit);
}

bool IsNontrivial(const NProto::TReadLimit& limit)
{
    return !IsTrivial(limit);
}

bool IsTrivial(const TReadLimit& limit)
{
    return limit.IsTrivial();
}

bool IsTrivial(const NProto::TReadLimit& limit)
{
    return
        !limit.has_row_index() &&
        !limit.has_key() &&
        !limit.has_offset() &&
        !limit.has_chunk_index();
}

void ToProto(NProto::TReadLimit* protoReadLimit, const TReadLimit& readLimit)
{
    protoReadLimit->CopyFrom(readLimit.AsProto());
}

void FromProto(TReadLimit* readLimit, const NProto::TReadLimit& protoReadLimit)
{
    *readLimit = protoReadLimit;
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TReadLimit& readLimit, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .DoIf(readLimit.HasKey(), [&] (TFluentMap fluent) {
                fluent.Item("key").Value(readLimit.GetKey());
            })
            .DoIf(readLimit.HasRowIndex(), [&] (TFluentMap fluent) {
                fluent.Item("row_index").Value(readLimit.GetRowIndex());
            })
            .DoIf(readLimit.HasOffset(), [&] (TFluentMap fluent) {
                fluent.Item("offset").Value(readLimit.GetOffset());
            })
            .DoIf(readLimit.HasChunkIndex(), [&] (TFluentMap fluent) {
                fluent.Item("chunk_index").Value(readLimit.GetChunkIndex());
            })
        .EndMap();
}

void Deserialize(TReadLimit& readLimit, INodePtr node)
{
    readLimit = TReadLimit();
    auto attributes = ConvertToAttributes(node);
    if (attributes->Contains("key")) {
        readLimit.SetKey(attributes->Get<TOwningKey>("key"));
    }
    if (attributes->Contains("row_index")) {
        readLimit.SetRowIndex(attributes->Get<i64>("row_index"));
    }
    if (attributes->Contains("offset")) {
        readLimit.SetOffset(attributes->Get<i64>("offset"));
    }
    if (attributes->Contains("chunk_index")) {
        readLimit.SetChunkIndex(attributes->Get<i64>("chunk_index"));
    }
}

////////////////////////////////////////////////////////////////////////////////

TReadRange::TReadRange()
    : LowerLimit_(TReadLimit())
    , UpperLimit_(TReadLimit())
{ }

TReadRange::TReadRange(const TReadLimit& exact)
    : LowerLimit_(exact)
    , UpperLimit_(exact.GetSuccessor())
{ }

TReadRange::TReadRange(const TReadLimit& lowerLimit, const TReadLimit& upperLimit)
    : LowerLimit_(lowerLimit)
    , UpperLimit_(upperLimit)
{ }

TReadRange::TReadRange(const NProto::TReadRange& range)
{
    InitCopy(range);
}

TReadRange::TReadRange(NProto::TReadRange&& range)
{
    InitMove(std::move(range));
}

TReadRange& TReadRange::operator= (const NProto::TReadRange& range)
{
    InitCopy(range);
    return *this;
}

TReadRange& TReadRange::operator= (NProto::TReadRange&& range)
{
    InitMove(std::move(range));
    return *this;
}

void TReadRange::InitCopy(const NProto::TReadRange& range)
{
    LowerLimit_ = range.has_lower_limit() ? TReadLimit(range.lower_limit()) : TReadLimit();
    UpperLimit_ = range.has_upper_limit() ? TReadLimit(range.upper_limit()) : TReadLimit();
}

void TReadRange::InitMove(NProto::TReadRange&& range)
{
    LowerLimit_ = range.has_lower_limit() ? TReadLimit(range.lower_limit()) : TReadLimit();
    UpperLimit_ = range.has_upper_limit() ? TReadLimit(range.upper_limit()) : TReadLimit();
}

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TReadRange& range)
{
    return Format("[<%v> : <%v>]", range.LowerLimit(), range.UpperLimit());
}

void ToProto(NProto::TReadRange* protoReadRange, const TReadRange& readRange)
{
    if (!readRange.LowerLimit().IsTrivial()) {
        *protoReadRange->mutable_lower_limit() = NYT::ToProto<NProto::TReadLimit>(readRange.LowerLimit());
    }
    if (!readRange.UpperLimit().IsTrivial()) {
        *protoReadRange->mutable_upper_limit() = NYT::ToProto<NProto::TReadLimit>(readRange.UpperLimit());
    }
}

void FromProto(TReadRange* readRange, const NProto::TReadRange& protoReadRange)
{
    *readRange = TReadRange(protoReadRange);
}

void Serialize(const TReadRange& readRange, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .DoIf(!readRange.LowerLimit().IsTrivial(), [&] (TFluentMap fluent) {
                fluent.Item("lower_limit").Value(readRange.LowerLimit());
            })
            .DoIf(!readRange.UpperLimit().IsTrivial(), [&] (TFluentMap fluent) {
                fluent.Item("upper_limit").Value(readRange.UpperLimit());
            })
        .EndMap();

}

void Deserialize(TReadRange& readRange, NYTree::INodePtr node)
{
    readRange = TReadRange();
    auto attributes = ConvertToAttributes(node);
    if (attributes->Contains("exact")) {
        if (attributes->Contains("lower_limit") || attributes->Contains("upper_limit")) {
            THROW_ERROR_EXCEPTION("\"lower_limit\" and \"upper_limit\" attributes cannot be specified if \"exact\" attribute is specified");
        }
        readRange.LowerLimit() = attributes->Get<TReadLimit>("exact");
        readRange.UpperLimit() = readRange.LowerLimit().GetSuccessor();
    }
    if (attributes->Contains("lower_limit")) {
        readRange.LowerLimit() = attributes->Get<TReadLimit>("lower_limit");
    }
    if (attributes->Contains("upper_limit")) {
        readRange.UpperLimit() = attributes->Get<TReadLimit>("upper_limit");
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
