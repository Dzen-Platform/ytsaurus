#pragma once

#include <yt/server/data_node/artifact.pb.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/scheduler/job.pb.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

struct TArtifactKey
    : public NProto::TArtifactKey
{
    TArtifactKey() = default;

    explicit TArtifactKey(const NChunkClient::TChunkId& id);
    explicit TArtifactKey(const NScheduler::NProto::TFileDescriptor& descriptor);

    // Hasher.
    operator size_t() const;

    // Comparer.
    bool operator == (const TArtifactKey& other) const;
};

Stroka ToString(const TArtifactKey& key);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

