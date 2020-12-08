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

TLegacyReadLimit::TLegacyReadLimit(const NProto::TReadLimit& protoLimit)
{
    InitCopy(protoLimit);
}

TLegacyReadLimit::TLegacyReadLimit(NProto::TReadLimit&& protoLimit)
{
    InitMove(std::move(protoLimit));
}

TLegacyReadLimit::TLegacyReadLimit(const std::unique_ptr<NProto::TReadLimit>& protoLimit)
{
    if (protoLimit) {
        InitCopy(*protoLimit);
    }
}

TLegacyReadLimit::TLegacyReadLimit(const TLegacyOwningKey& key)
{
    SetLegacyKey(key);
}

TLegacyReadLimit::TLegacyReadLimit(TLegacyOwningKey&& key)
{
    SetLegacyKey(std::move(key));
}

TLegacyReadLimit& TLegacyReadLimit::operator= (const NProto::TReadLimit& protoLimit)
{
    InitCopy(protoLimit);
    return *this;
}

TLegacyReadLimit& TLegacyReadLimit::operator= (NProto::TReadLimit&& protoLimit)
{
    InitMove(std::move(protoLimit));
    return *this;
}

TLegacyReadLimit TLegacyReadLimit::GetSuccessor() const
{
    TLegacyReadLimit result;
    if (HasLegacyKey()) {
        auto key = GetLegacyKey();
        result.SetLegacyKey(GetKeyPrefixSuccessor(key, key.GetCount()));
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

const NProto::TReadLimit& TLegacyReadLimit::AsProto() const
{
    return ReadLimit_;
}

const TLegacyOwningKey& TLegacyReadLimit::GetLegacyKey() const
{
    YT_ASSERT(HasLegacyKey());
    return Key_;
}

bool TLegacyReadLimit::HasLegacyKey() const
{
    return ReadLimit_.has_legacy_key();
}

TLegacyReadLimit& TLegacyReadLimit::SetLegacyKey(const TLegacyOwningKey& key)
{
    Key_ = key;
    ToProto(ReadLimit_.mutable_legacy_key(), Key_);
    return *this;
}

TLegacyReadLimit& TLegacyReadLimit::SetLegacyKey(TLegacyOwningKey&& key)
{
    swap(Key_, key);
    ToProto(ReadLimit_.mutable_legacy_key(), Key_);
    return *this;
}

i64 TLegacyReadLimit::GetRowIndex() const
{
    YT_ASSERT(HasRowIndex());
    return ReadLimit_.row_index();
}

bool TLegacyReadLimit::HasRowIndex() const
{
    return ReadLimit_.has_row_index();
}

TLegacyReadLimit& TLegacyReadLimit::SetRowIndex(i64 rowIndex)
{
    ReadLimit_.set_row_index(rowIndex);
    return *this;
}

i64 TLegacyReadLimit::GetOffset() const
{
    YT_ASSERT(HasOffset());
    return ReadLimit_.offset();
}

bool TLegacyReadLimit::HasOffset() const
{
    return ReadLimit_.has_offset();
}

TLegacyReadLimit& TLegacyReadLimit::SetOffset(i64 offset)
{
    ReadLimit_.set_offset(offset);
    return *this;
}

i64 TLegacyReadLimit::GetChunkIndex() const
{
    YT_ASSERT(HasChunkIndex());
    return ReadLimit_.chunk_index();
}

bool TLegacyReadLimit::HasChunkIndex() const
{
    return ReadLimit_.has_chunk_index();
}

TLegacyReadLimit& TLegacyReadLimit::SetChunkIndex(i64 chunkIndex)
{
    ReadLimit_.set_chunk_index(chunkIndex);
    return *this;
}

i32 TLegacyReadLimit::GetTabletIndex() const
{
    YT_ASSERT(HasTabletIndex());
    return ReadLimit_.tablet_index();
}

bool TLegacyReadLimit::HasTabletIndex() const
{
    return ReadLimit_.has_tablet_index();
}

TLegacyReadLimit& TLegacyReadLimit::SetTabletIndex(i32 tabletIndex)
{
    ReadLimit_.set_tablet_index(tabletIndex);
    return *this;
}

bool TLegacyReadLimit::IsTrivial() const
{
    return NChunkClient::IsTrivial(ReadLimit_);
}

void TLegacyReadLimit::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, ReadLimit_);
    Persist(context, Key_);
}

void TLegacyReadLimit::MergeLowerLegacyKey(const TLegacyOwningKey& key)
{
    if (!HasLegacyKey() || GetLegacyKey() < key) {
        SetLegacyKey(key);
    }
}

void TLegacyReadLimit::MergeUpperLegacyKey(const TLegacyOwningKey& key)
{
    if (!HasLegacyKey() || GetLegacyKey() > key) {
        SetLegacyKey(key);
    }
}

void TLegacyReadLimit::MergeLowerRowIndex(i64 rowIndex)
{
    if (!HasRowIndex() || GetRowIndex() < rowIndex) {
        SetRowIndex(rowIndex);
    }
}

void TLegacyReadLimit::MergeUpperRowIndex(i64 rowIndex)
{
    if (!HasRowIndex() || GetRowIndex() > rowIndex) {
        SetRowIndex(rowIndex);
    }
}

void TLegacyReadLimit::InitKey()
{
    if (ReadLimit_.has_legacy_key()) {
        FromProto(&Key_, ReadLimit_.legacy_key());
    }
}

void TLegacyReadLimit::InitCopy(const NProto::TReadLimit& readLimit)
{
    ReadLimit_.CopyFrom(readLimit);
    InitKey();
}

void TLegacyReadLimit::InitMove(NProto::TReadLimit&& readLimit)
{
    ReadLimit_.Swap(&readLimit);
    InitKey();
}

size_t TLegacyReadLimit::SpaceUsed() const
{
    return
       sizeof(*this) +
       ReadLimit_.SpaceUsed() - sizeof(ReadLimit_) +
       Key_.GetSpaceUsed() - sizeof(Key_);
}

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TLegacyReadLimit& limit)
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

    if (limit.HasLegacyKey()) {
        append("Key", ToString(limit.GetLegacyKey()));
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

bool IsTrivial(const TLegacyReadLimit& limit)
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

void ToProto(NProto::TReadLimit* protoReadLimit, const TLegacyReadLimit& readLimit)
{
    protoReadLimit->CopyFrom(readLimit.AsProto());
}

void FromProto(TLegacyReadLimit* readLimit, const NProto::TReadLimit& protoReadLimit)
{
    *readLimit = protoReadLimit;
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TLegacyReadLimit& readLimit, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .DoIf(readLimit.HasLegacyKey(), [&] (TFluentMap fluent) {
                fluent.Item("key").Value(readLimit.GetLegacyKey());
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

void Deserialize(TLegacyReadLimit& readLimit, INodePtr node)
{
    if (node->GetType() != NYTree::ENodeType::Map) {
        THROW_ERROR_EXCEPTION("Error parsing read limit: expected %Qlv, actual %Qlv",
            NYTree::ENodeType::Map,
            node->GetType());
    }

    readLimit = TLegacyReadLimit();
    auto attributes = ConvertToAttributes(node);

    auto optionalKey = FindReadLimitComponent<TLegacyOwningKey>(attributes, "key");
    if (optionalKey) {
        readLimit.SetLegacyKey(*optionalKey);
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

TLegacyReadRange::TLegacyReadRange(const TLegacyReadLimit& exact)
    : LowerLimit_(exact)
    , UpperLimit_(exact.GetSuccessor())
{ }

TLegacyReadRange::TLegacyReadRange(const TLegacyReadLimit& lowerLimit, const TLegacyReadLimit& upperLimit)
    : LowerLimit_(lowerLimit)
    , UpperLimit_(upperLimit)
{ }

TLegacyReadRange::TLegacyReadRange(const NProto::TReadRange& range)
{
    InitCopy(range);
}

TLegacyReadRange::TLegacyReadRange(NProto::TReadRange&& range)
{
    InitMove(std::move(range));
}

TLegacyReadRange& TLegacyReadRange::operator= (const NProto::TReadRange& range)
{
    InitCopy(range);
    return *this;
}

TLegacyReadRange& TLegacyReadRange::operator= (NProto::TReadRange&& range)
{
    InitMove(std::move(range));
    return *this;
}

void TLegacyReadRange::InitCopy(const NProto::TReadRange& range)
{
    LowerLimit_ = range.has_lower_limit() ? TLegacyReadLimit(range.lower_limit()) : TLegacyReadLimit();
    UpperLimit_ = range.has_upper_limit() ? TLegacyReadLimit(range.upper_limit()) : TLegacyReadLimit();
}

void TLegacyReadRange::InitMove(NProto::TReadRange&& range)
{
    LowerLimit_ = range.has_lower_limit() ? TLegacyReadLimit(range.lower_limit()) : TLegacyReadLimit();
    UpperLimit_ = range.has_upper_limit() ? TLegacyReadLimit(range.upper_limit()) : TLegacyReadLimit();
}

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TLegacyReadRange& range)
{
    return Format("[<%v> : <%v>]", range.LowerLimit(), range.UpperLimit());
}

void ToProto(NProto::TReadRange* protoReadRange, const TLegacyReadRange& readRange)
{
    if (!readRange.LowerLimit().IsTrivial()) {
        ToProto(protoReadRange->mutable_lower_limit(), readRange.LowerLimit());
    }
    if (!readRange.UpperLimit().IsTrivial()) {
        ToProto(protoReadRange->mutable_upper_limit(), readRange.UpperLimit());
    }
}

void FromProto(TLegacyReadRange* readRange, const NProto::TReadRange& protoReadRange)
{
    *readRange = TLegacyReadRange(protoReadRange);
}

void Serialize(const TLegacyReadRange& readRange, NYson::IYsonConsumer* consumer)
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

void Deserialize(TLegacyReadRange& readRange, NYTree::INodePtr node)
{
    if (node->GetType() != NYTree::ENodeType::Map) {
        THROW_ERROR_EXCEPTION("Error parsing read range: expected %Qlv, actual %Qlv",
            NYTree::ENodeType::Map,
            node->GetType());
    }

    readRange = TLegacyReadRange();
    auto attributes = ConvertToAttributes(node);
    auto optionalExact = FindReadRangeComponent<TLegacyReadLimit>(attributes, "exact");
    auto optionalLowerLimit = FindReadRangeComponent<TLegacyReadLimit>(attributes, "lower_limit");
    auto optionalUpperLimit = FindReadRangeComponent<TLegacyReadLimit>(attributes, "upper_limit");

    if (optionalExact) {
        if (optionalLowerLimit || optionalUpperLimit) {
            THROW_ERROR_EXCEPTION("\"lower_limit\" and \"upper_limit\" attributes cannot be specified "
                "together with \"exact\" attribute");
        }
        readRange = TLegacyReadRange(*optionalExact);
    }

    if (optionalLowerLimit) {
        readRange.LowerLimit() = *optionalLowerLimit;
    }

    if (optionalUpperLimit) {
        readRange.UpperLimit() = *optionalUpperLimit;
    }
}

void TLegacyReadRange::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, LowerLimit_);
    Persist(context, UpperLimit_);
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TReadLimit, /*key*/4, TUnversionedOwningRow)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
