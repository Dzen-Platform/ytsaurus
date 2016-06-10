#pragma once

#include "public.h"

#include <yt/core/compression/public.h>

#include <yt/core/misc/nullable.h>

namespace NYT {
namespace NHydra {

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
    const Stroka& fileName,
    int snapshotId,
    bool raw,
    i64 offset = -1);

ISnapshotWriterPtr CreateFileSnapshotWriter(
    const Stroka& fileName,
    NCompression::ECodec codec,
    int snapshotId,
    const NProto::TSnapshotMeta& meta,
    bool raw);

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
