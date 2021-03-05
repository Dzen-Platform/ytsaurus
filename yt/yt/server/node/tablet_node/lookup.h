#pragma once

#include "public.h"

#include <yt/yt/ytlib/tablet_client/public.h>

#include <yt/yt/core/actions/public.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

//! Executes a bunch of row lookup requests. Request parameters are parsed via #reader,
//! response is written into #writer.
void LookupRows(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    bool useLookupCache,
    const NChunkClient::TClientBlockReadOptions& blockReadOptions,
    NTableClient::TWireProtocolReader* reader,
    NTableClient::TWireProtocolWriter* writer);

void VersionedLookupRows(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    bool useLookupCache,
    const NChunkClient::TClientBlockReadOptions& blockReadOptions,
    NTableClient::TRetentionConfigPtr retentionConfig,
    NTableClient::TWireProtocolReader* reader,
    NTableClient::TWireProtocolWriter* writer);

void LookupRead(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    bool useLookupCache,
    const NChunkClient::TClientBlockReadOptions& blockReadOptions,
    NTableClient::TRetentionConfigPtr retentionConfig,
    NTableClient::TWireProtocolReader* reader,
    NTableClient::TWireProtocolWriter* writer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
