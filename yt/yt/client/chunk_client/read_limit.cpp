#include "read_limit.h"

#include <yt/core/misc/format.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/yson/protobuf_interop.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node.h>

namespace NYT::NChunkClient {

using namespace NYTree;
using namespace NYson;
using namespace NTableClient;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

TReadLimit::TReadLimit(const NProto::TReadLimit& protoLimit)
{
    InitCopy(protoLimit);
}

TReadLimit::TReadLimit(NProto::TReadLimit&& protoLimit)
{
    InitMove(std::move(protoLimit));
}

TReadLimit::TReadLimit(const std::unique_ptr<NProto::TReadLimit>& protoLimit)
{
    if (protoLimit) {
        InitCopy(*protoLimit);
    }
}

TReadLimit::TReadLimit(const TLegacyOwningKey& key)
{
    SetKey(key);
}

TReadLimit::TReadLimit(TLegacyOwningKey&& key)
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
        result.SetKey(GetKeyPrefixSuccessor(key, key.GetCount()));
    }
    if (HasRowIndex()) {
        result.SetRowIndex(GetRowIndex() + 1);
    }
    if (HasChunkIndex()) {
        result.SetChunkIndex(GetChunkIndex() + 1);
    }
    if (HasTabletIndex()) {
        // We use tabletIndex in ordered dynamic tables, where indexing is over pairs (tabletIndex, rowIndex).
        result.SetTabletIndex(GetTabletIndex());
    }
    return result;
}

const NProto::TReadLimit& TReadLimit::AsProto() const
{
    return ReadLimit_;
}

const TLegacyOwningKey& TReadLimit::GetKey() const
{
    YT_ASSERT(HasKey());
    return Key_;
}

bool TReadLimit::HasKey() const
{
    return ReadLimit_.has_legacy_key();
}

TReadLimit& TReadLimit::SetKey(const TLegacyOwningKey& key)
{
    Key_ = key;
    ToProto(ReadLimit_.mutable_legacy_key(), Key_);
    return *this;
}

TReadLimit& TReadLimit::SetKey(TLegacyOwningKey&& key)
{
    swap(Key_, key);
    ToProto(ReadLimit_.mutable_legacy_key(), Key_);
    return *this;
}

i64 TReadLimit::GetRowIndex() const
{
    YT_ASSERT(HasRowIndex());
    return ReadLimit_.row_index();
}

bool TReadLimit::HasRowIndex() const
{
    return ReadLimit_.has_row_index();
}

TReadLimit& TReadLimit::SetRowIndex(i64 rowIndex)
{
    ReadLimit_.set_row_index(rowIndex);
    return *this;
}

i64 TReadLimit::GetOffset() const
{
    YT_ASSERT(HasOffset());
    return ReadLimit_.offset();
}

bool TReadLimit::HasOffset() const
{
    return ReadLimit_.has_offset();
}

TReadLimit& TReadLimit::SetOffset(i64 offset)
{
    ReadLimit_.set_offset(offset);
    return *this;
}

i64 TReadLimit::GetChunkIndex() const
{
    YT_ASSERT(HasChunkIndex());
    return ReadLimit_.chunk_index();
}

bool TReadLimit::HasChunkIndex() const
{
    return ReadLimit_.has_chunk_index();
}

TReadLimit& TReadLimit::SetChunkIndex(i64 chunkIndex)
{
    ReadLimit_.set_chunk_index(chunkIndex);
    return *this;
}

i32 TReadLimit::GetTabletIndex() const
{
    YT_ASSERT(HasTabletIndex());
    return ReadLimit_.tablet_index();
}

bool TReadLimit::HasTabletIndex() const
{
    return ReadLimit_.has_tablet_index();
}

TReadLimit& TReadLimit::SetTabletIndex(i32 tabletIndex)
{
    ReadLimit_.set_tablet_index(tabletIndex);
    return *this;
}

bool TReadLimit::IsTrivial() const
{
    return NChunkClient::IsTrivial(ReadLimit_);
}

void TReadLimit::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, ReadLimit_);
    Persist(context, Key_);
}

void TReadLimit::MergeLowerKey(const TLegacyOwningKey& key)
{
    if (!HasKey() || GetKey() < key) {
        SetKey(key);
    }
}

void TReadLimit::MergeUpperKey(const TLegacyOwningKey& key)
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

void TReadLimit::InitKey()
{
    if (ReadLimit_.has_legacy_key()) {
        FromProto(&Key_, ReadLimit_.legacy_key());
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

TString ToString(const TReadLimit& limit)
{
    using ::ToString;

    TStringBuilder builder;
    builder.AppendChar('{');

    bool firstToken = true;
    auto append = [&] (const char* label, TStringBuf value) {
        if (!firstToken) {
            builder.AppendString(", ");
        }
        firstToken = false;
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
        append("ChunkIndex", ToString(limit.GetChunkIndex()));
    }

    if (limit.HasTabletIndex()) {
        append("TabletIndex", ToString(limit.GetTabletIndex()));
    }

    builder.AppendChar('}');
    return builder.Flush();
}

bool IsTrivial(const TReadLimit& limit)
{
    return limit.IsTrivial();
}

bool IsTrivial(const NProto::TReadLimit& limit)
{
    return
        !limit.has_row_index() &&
        !limit.has_legacy_key() &&
        !limit.has_offset() &&
        !limit.has_chunk_index() &&
        !limit.has_tablet_index();
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
            .DoIf(readLimit.HasTabletIndex(), [&] (TFluentMap fluent) {
                fluent.Item("tablet_index").Value(readLimit.GetTabletIndex());
            })
        .EndMap();
}

namespace {

template <class T>
std::optional<T> FindReadLimitComponent(const IAttributeDictionaryPtr& attributes, const TString& key)
{
    try {
        return attributes->Find<T>(key);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing %Qv component of a read limit",
            key)
            << ex;
    }
}

} // namespace

void Deserialize(TReadLimit& readLimit, INodePtr node)
{
    if (node->GetType() != NYTree::ENodeType::Map) {
        THROW_ERROR_EXCEPTION("Error parsing read limit: expected %Qlv, actual %Qlv",
            NYTree::ENodeType::Map,
            node->GetType());
    }

    readLimit = TReadLimit();
    auto attributes = ConvertToAttributes(node);

    auto optionalKey = FindReadLimitComponent<TLegacyOwningKey>(attributes, "key");
    if (optionalKey) {
        readLimit.SetKey(*optionalKey);
    }

    auto optionalRowIndex = FindReadLimitComponent<i64>(attributes, "row_index");
    if (optionalRowIndex) {
        readLimit.SetRowIndex(*optionalRowIndex);
    }

    auto optionalOffset = FindReadLimitComponent<i64>(attributes, "offset");
    if (optionalOffset) {
        readLimit.SetOffset(*optionalOffset);
    }

    auto optionalChunkIndex = FindReadLimitComponent<i64>(attributes, "chunk_index");
    if (optionalChunkIndex) {
        readLimit.SetChunkIndex(*optionalChunkIndex);
    }

    auto optionalTabletIndex = FindReadLimitComponent<i32>(attributes, "tablet_index");
    if (optionalTabletIndex) {
        readLimit.SetTabletIndex(*optionalTabletIndex);
    }
}

////////////////////////////////////////////////////////////////////////////////

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

TString ToString(const TReadRange& range)
{
    return Format("[<%v> : <%v>]", range.LowerLimit(), range.UpperLimit());
}

void ToProto(NProto::TReadRange* protoReadRange, const TReadRange& readRange)
{
    if (!readRange.LowerLimit().IsTrivial()) {
        ToProto(protoReadRange->mutable_lower_limit(), readRange.LowerLimit());
    }
    if (!readRange.UpperLimit().IsTrivial()) {
        ToProto(protoReadRange->mutable_upper_limit(), readRange.UpperLimit());
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

namespace {

template <class T>
std::optional<T> FindReadRangeComponent(const IAttributeDictionaryPtr& attributes, const TString& key)
{
    try {
        return attributes->Find<T>(key);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing %Qv component of a read range",
            key)
            << ex;
    }
}

} // namespace

void Deserialize(TReadRange& readRange, NYTree::INodePtr node)
{
    if (node->GetType() != NYTree::ENodeType::Map) {
        THROW_ERROR_EXCEPTION("Error parsing read range: expected %Qlv, actual %Qlv",
            NYTree::ENodeType::Map,
            node->GetType());
    }

    readRange = TReadRange();
    auto attributes = ConvertToAttributes(node);
    auto optionalExact = FindReadRangeComponent<TReadLimit>(attributes, "exact");
    auto optionalLowerLimit = FindReadRangeComponent<TReadLimit>(attributes, "lower_limit");
    auto optionalUpperLimit = FindReadRangeComponent<TReadLimit>(attributes, "upper_limit");

    if (optionalExact) {
        if (optionalLowerLimit || optionalUpperLimit) {
            THROW_ERROR_EXCEPTION("\"lower_limit\" and \"upper_limit\" attributes cannot be specified "
                "together with \"exact\" attribute");
        }
        readRange = TReadRange(*optionalExact);
    }

    if (optionalLowerLimit) {
        readRange.LowerLimit() = *optionalLowerLimit;
    }

    if (optionalUpperLimit) {
        readRange.UpperLimit() = *optionalUpperLimit;
    }
}

void TReadRange::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, LowerLimit_);
    Persist(context, UpperLimit_);
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TReadLimit, /*key*/4, TUnversionedOwningRow)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
