#pragma once

#include "public.h"

#include <yt/yt/core/compression/public.h>

#include <yt/yt/core/misc/optional.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotStore
    : public TRefCounted
{
public:
    explicit TFileSnapshotStore(TLocalSnapshotStoreConfigPtr config);
    void Initialize();

    ~TFileSnapshotStore();

    bool CheckSnapshotExists(int snapshotId);
    int GetLatestSnapshotId(int maxSnapshotId);

    ISnapshotReaderPtr CreateReader(int snapshotId);
    ISnapshotReaderPtr CreateRawReader(int snapshotId, i64 offset);

    ISnapshotWriterPtr CreateWriter(int snapshotId, const NProto::TSnapshotMeta& meta);
    ISnapshotWriterPtr CreateRawWriter(int snapshotId);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TFileSnapshotStore)

////////////////////////////////////////////////////////////////////////////////

ISnapshotReaderPtr CreateFileSnapshotReader(
    const TString& fileName,
    int snapshotId,
    bool raw,
    std::optional<i64> offset = std::nullopt,
    bool skipHeader = false);

ISnapshotWriterPtr CreateFileSnapshotWriter(
    const TString& fileName,
    NCompression::ECodec codec,
    int snapshotId,
    const NProto::TSnapshotMeta& meta,
    bool raw);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
