#pragma once

#include "public.h"

#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/ytlib/tablet_client/wire_protocol.h>

namespace NYT {
namespace NChunkClient {

// ToDo(psushin): move to NTableClient.

////////////////////////////////////////////////////////////////////////////////

class TKeySetWriter
    : public TRefCounted
{
public:
    int WriteKey(const NTableClient::TKey& key);
    int WriteValueRange(TRange<NTableClient::TUnversionedValue> key);

    TSharedRef Finish();

private:
    NTabletClient::TWireProtocolWriter WireProtocolWriter_;
    int Index_ = 0;
};

DEFINE_REFCOUNTED_TYPE(TKeySetWriter)

////////////////////////////////////////////////////////////////////////////////

class TKeySetReader
{
public:
    TKeySetReader(const TSharedRef& compressedData);

    const TRange<NTableClient::TKey> GetKeys() const;

private:
    NTabletClient::TWireProtocolReader WireProtocolReader_;
    std::vector<NTableClient::TKey> Keys_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
