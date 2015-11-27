#pragma once

#include "public.h"

#include <ytlib/chunk_client/schema.pb.h>

#include <ytlib/table_client/unversioned_row.h>

#include <core/misc/phoenix.h>

#include <core/ytree/public.h>
#include <core/yson/consumer.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TReadLimit
{
public:
    TReadLimit();

    explicit TReadLimit(const NProto::TReadLimit& readLimit);
    explicit TReadLimit(NProto::TReadLimit&& readLimit);

    explicit TReadLimit(const NTableClient::TOwningKey& key);
    explicit TReadLimit(NTableClient::TOwningKey&& key);

    TReadLimit& operator= (const NProto::TReadLimit& protoLimit);
    TReadLimit& operator= (NProto::TReadLimit&& protoLimit);

    TReadLimit GetSuccessor() const;

    const NProto::TReadLimit& AsProto() const;

    const NTableClient::TOwningKey& GetKey() const;
    bool HasKey() const;
    void SetKey(const NTableClient::TOwningKey& key);
    void SetKey(NTableClient::TOwningKey&& key);

    i64 GetRowIndex() const;
    bool HasRowIndex() const;
    void SetRowIndex(i64 rowIndex);

    i64 GetOffset() const;
    bool HasOffset() const;
    void SetOffset(i64 offset);

    i64 GetChunkIndex() const;
    bool HasChunkIndex() const;
    void SetChunkIndex(i64 chunkIndex);

    bool IsTrivial() const;

    void MergeLowerLimit(const NProto::TReadLimit& readLimit);
    void MergeUpperLimit(const NProto::TReadLimit& readLimit);

    void Persist(NPhoenix::TPersistenceContext& context);

    size_t SpaceUsedExcludingSelf() const;

private:
    NProto::TReadLimit ReadLimit_;
    NTableClient::TOwningKey Key_;

    void InitKey();
    void InitCopy(const NProto::TReadLimit& readLimit);
    void InitMove(NProto::TReadLimit&& readLimit);

};

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TReadLimit& limit);

bool IsTrivial(const TReadLimit& limit);
bool IsTrivial(const NProto::TReadLimit& limit);

bool IsNontrivial(const TReadLimit& limit);
bool IsNontrivial(const NProto::TReadLimit& limit);

void ToProto(NProto::TReadLimit* protoReadLimit, const TReadLimit& readLimit);
void FromProto(TReadLimit* readLimit, const NProto::TReadLimit& protoReadLimit);

void Serialize(const TReadLimit& readLimit, NYson::IYsonConsumer* consumer);
void Deserialize(TReadLimit& readLimit, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

class TReadRange
{
public:
    TReadRange();
    TReadRange(const TReadLimit& lowerLimit, const TReadLimit& upperLimit);
    explicit TReadRange(const TReadLimit& exact);

    explicit TReadRange(const NProto::TReadRange& range);
    explicit TReadRange(NProto::TReadRange&& range);
    TReadRange& operator= (const NProto::TReadRange& range);
    TReadRange& operator= (NProto::TReadRange&& range);

    DEFINE_BYREF_RW_PROPERTY(TReadLimit, LowerLimit);
    DEFINE_BYREF_RW_PROPERTY(TReadLimit, UpperLimit);

private:
    void InitCopy(const NProto::TReadRange& range);
    void InitMove(NProto::TReadRange&& range);
};

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TReadRange& range);

void ToProto(NProto::TReadRange* protoReadRange, const TReadRange& readRange);
void FromProto(TReadRange* readRange, const NProto::TReadRange& protoReadRange);

void Serialize(const TReadRange& readRange, NYson::IYsonConsumer* consumer);
void Deserialize(TReadRange& readRange, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
