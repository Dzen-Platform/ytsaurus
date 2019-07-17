#include "chunk_replica.h"

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/erasure/public.h>

#include <yt/core/misc/format.h>

namespace NYT::NChunkClient {

using namespace NNodeTrackerClient;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

TString ToString(TChunkReplicaWithMedium replica)
{
    if (replica.GetReplicaIndex() == GenericChunkReplicaIndex) {
        return Format("%v@%v", replica.GetNodeId(), replica.GetMediumIndex());
    } else {
        return Format("%v/%v@%v",
            replica.GetNodeId(),
            replica.GetReplicaIndex(),
            replica.GetMediumIndex());
    }
}

TString ToString(TChunkReplica replica)
{
    if (replica.GetReplicaIndex() == GenericChunkReplicaIndex) {
        return Format("%v", replica.GetNodeId());
    } else {
        return Format("%v/%v",
            replica.GetNodeId(),
            replica.GetReplicaIndex());
    }
}

TString ToString(const TChunkIdWithIndex& id)
{
    if (id.ReplicaIndex == GenericChunkReplicaIndex) {
        return ToString(id.Id);
    } else if (TypeFromId(id.Id) == EObjectType::JournalChunk) {
        return Format("%v/%v",
            id.Id,
            EJournalReplicaType(id.ReplicaIndex));
    } else {
        return Format("%v/%v",
            id.Id,
            id.ReplicaIndex);
    }
}

TString ToString(const TChunkIdWithIndexes& id)
{
    if (TypeFromId(id.Id) == EObjectType::JournalChunk) {
        return Format("%v/%v",
            id.Id,
            EJournalReplicaType(id.ReplicaIndex));
    } else {
        return Format("%v/%v@%v",
            id.Id,
            id.ReplicaIndex,
            id.MediumIndex);
    }
}

////////////////////////////////////////////////////////////////////////////////

TChunkReplicaAddressFormatter::TChunkReplicaAddressFormatter(TNodeDirectoryPtr nodeDirectory)
    : NodeDirectory_(std::move(nodeDirectory))
{ }

void TChunkReplicaAddressFormatter::operator()(TStringBuilderBase* builder, TChunkReplicaWithMedium replica) const
{
    const auto* descriptor = NodeDirectory_->FindDescriptor(replica.GetNodeId());
    if (descriptor) {
        builder->AppendFormat(
            "%v/%v@%v",
            descriptor->GetDefaultAddress(),
            replica.GetReplicaIndex(),
            replica.GetMediumIndex());
    } else {
        builder->AppendFormat(
            "<unresolved-%v>/%v@%v",
            replica.GetNodeId(),
            replica.GetReplicaIndex(),
            replica.GetMediumIndex());
    }
}

void TChunkReplicaAddressFormatter::operator()(TStringBuilderBase* builder, TChunkReplica replica) const
{
    const auto* descriptor = NodeDirectory_->FindDescriptor(replica.GetNodeId());
    if (descriptor) {
        builder->AppendFormat(
            "%v/%v",
            descriptor->GetDefaultAddress(),
            replica.GetReplicaIndex());
    } else {
        builder->AppendFormat(
            "<unresolved-%v>/%v",
            replica.GetNodeId(),
            replica.GetReplicaIndex());
    }
}

////////////////////////////////////////////////////////////////////////////////

bool IsArtifactChunkId(TChunkId id)
{
    return TypeFromId(id) == EObjectType::Artifact;
}

bool IsJournalChunkId(TChunkId id)
{
    return TypeFromId(id) == EObjectType::JournalChunk;
}

bool IsErasureChunkId(TChunkId id)
{
    return TypeFromId(id) == EObjectType::ErasureChunk;
}

bool IsErasureChunkPartId(TChunkId id)
{
    auto type = TypeFromId(id);
    return
        type >= EObjectType::ErasureChunkPart_0 &&
        type <= EObjectType::ErasureChunkPart_15;
}

TChunkId ErasurePartIdFromChunkId(TChunkId id, int index)
{
    return ReplaceTypeInId(id, EObjectType(static_cast<int>(EObjectType::ErasureChunkPart_0) + index));
}

TChunkId ErasureChunkIdFromPartId(TChunkId id)
{
    return ReplaceTypeInId(id, EObjectType::ErasureChunk);
}

int IndexFromErasurePartId(TChunkId id)
{
    int index = static_cast<int>(TypeFromId(id)) - static_cast<int>(EObjectType::ErasureChunkPart_0);
    YT_VERIFY(index >= 0 && index <= 15);
    return index;
}

TChunkId EncodeChunkId(const TChunkIdWithIndex& idWithIndex)
{
    return IsErasureChunkId(idWithIndex.Id)
        ? ErasurePartIdFromChunkId(idWithIndex.Id, idWithIndex.ReplicaIndex)
        : idWithIndex.Id;
}

TChunkIdWithIndex DecodeChunkId(TChunkId id)
{
    return IsErasureChunkPartId(id)
        ? TChunkIdWithIndex(ErasureChunkIdFromPartId(id), IndexFromErasurePartId(id))
        : TChunkIdWithIndex(id, GenericChunkReplicaIndex);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
