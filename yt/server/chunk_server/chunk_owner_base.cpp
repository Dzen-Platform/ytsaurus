#include "chunk_owner_base.h"
#include "chunk_list.h"
#include "helpers.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/cluster_resources.h>

#include <yt/client/chunk_client/data_statistics.h>

#include <yt/ytlib/chunk_client/helpers.h>

namespace NYT::NChunkServer {

using namespace NCrypto;
using namespace NYTree;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressClient;
using namespace NCypressClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TChunkOwnerBase::TChunkOwnerBase(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
{
    Replication_.SetVital(true);
    for (auto& policy : Replication_) {
        policy.Clear();
    }
    if (IsTrunk()) {
        CompressionCodec_.Set(NCompression::ECodec::None);
        ErasureCodec_.Set(NErasure::ECodec::None);
    }
}

void TChunkOwnerBase::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);

    using NYT::Save;
    Save(context, ChunkList_);
    Save(context, UpdateMode_);
    Save(context, Replication_);
    Save(context, PrimaryMediumIndex_);
    Save(context, SnapshotStatistics_);
    Save(context, DeltaStatistics_);
    Save(context, CompressionCodec_);
    Save(context, ErasureCodec_);
}

void TChunkOwnerBase::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);

    using NYT::Load;
    Load(context, ChunkList_);
    Load(context, UpdateMode_);
    Load(context, Replication_);
    Load(context, PrimaryMediumIndex_);
    Load(context, SnapshotStatistics_);
    Load(context, DeltaStatistics_);
    Load(context, CompressionCodec_);
    Load(context, ErasureCodec_);
}

const TChunkList* TChunkOwnerBase::GetSnapshotChunkList() const
{
    switch (UpdateMode_) {
        case EUpdateMode::None:
        case EUpdateMode::Overwrite:
            return ChunkList_;

        case EUpdateMode::Append:
            if (GetType() == EObjectType::Journal) {
                return ChunkList_;
            } else {
                const auto& children = ChunkList_->Children();
                YCHECK(children.size() == 2);
                return children[0]->AsChunkList();
            }

        default:
            Y_UNREACHABLE();
    }
}

const TChunkList* TChunkOwnerBase::GetDeltaChunkList() const
{
    switch (UpdateMode_) {
        case EUpdateMode::Append:
            if (GetType() == EObjectType::Journal) {
                return ChunkList_;
            } else {
                const auto& children = ChunkList_->Children();
                YCHECK(children.size() == 2);
                return children[1]->AsChunkList();
            }

        case EUpdateMode::Overwrite:
            return ChunkList_;

        default:
            Y_UNREACHABLE();
    }
}

void TChunkOwnerBase::BeginUpload(EUpdateMode mode)
{
    UpdateMode_ = mode;
}

void TChunkOwnerBase::EndUpload(
    const TDataStatistics* statistics,
    const NTableServer::TSharedTableSchemaPtr& /*sharedSchema*/,
    NTableClient::ETableSchemaMode /*schemaMode*/,
    std::optional<NTableClient::EOptimizeFor> /*optimizeFor*/,
    const std::optional<TMD5Hasher>& /*md5Hasher*/)
{
    std::optional<TDataStatistics> updateStatistics;

    if (!IsExternal()) {
        updateStatistics = ComputeUpdateStatistics();
    }

    if (statistics && updateStatistics) {
        YCHECK(*statistics == *updateStatistics);
    }

    if (statistics) {
        switch (UpdateMode_) {
            case EUpdateMode::Append:
                DeltaStatistics_ = *statistics;
                break;

            case EUpdateMode::Overwrite:
                SnapshotStatistics_ = *statistics;
                break;

            default:
                Y_UNREACHABLE();
        }
    }
}

void TChunkOwnerBase::GetUploadParams(std::optional<TMD5Hasher>* /*md5Hasher*/)
{ }

bool TChunkOwnerBase::IsSorted() const
{
    return false;
}

ENodeType TChunkOwnerBase::GetNodeType() const
{
    return ENodeType::Entity;
}

TDataStatistics TChunkOwnerBase::ComputeTotalStatistics() const
{
    return SnapshotStatistics_ + DeltaStatistics_;
}

TDataStatistics TChunkOwnerBase::ComputeUpdateStatistics() const
{
    YCHECK(!IsExternal());

    switch (UpdateMode_) {
        case EUpdateMode::Append:
            return GetDeltaChunkList()->Statistics().ToDataStatistics();

        case EUpdateMode::Overwrite:
            return GetSnapshotChunkList()->Statistics().ToDataStatistics();

        default:
            Y_UNREACHABLE();
    }
}

bool TChunkOwnerBase::HasDataWeight() const
{
    return !HasInvalidDataWeight(SnapshotStatistics_) && !HasInvalidDataWeight(DeltaStatistics_);
}

NSecurityServer::TClusterResources TChunkOwnerBase::GetTotalResourceUsage() const
{
    return TBase::GetTotalResourceUsage() + GetDiskUsage(ComputeTotalStatistics());
}

NSecurityServer::TClusterResources TChunkOwnerBase::GetDeltaResourceUsage() const
{
    TDataStatistics statistics;
    if (IsTrunk()) {
        statistics = DeltaStatistics_ + SnapshotStatistics_;
    } else {
        switch (UpdateMode_) {
            case EUpdateMode::Append:
                statistics = DeltaStatistics_;
                break;
            case EUpdateMode::Overwrite:
                statistics = SnapshotStatistics_;
                break;
            default:
                break; // Leave statistics empty - this is a newly branched node.
        }
    }
    return TBase::GetDeltaResourceUsage() + GetDiskUsage(statistics);
}

NSecurityServer::TClusterResources TChunkOwnerBase::GetDiskUsage(const TDataStatistics& statistics) const
{
    NSecurityServer::TClusterResources result;
    for (auto mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        result.DiskSpace[mediumIndex] = CalculateDiskSpaceUsage(
            Replication()[mediumIndex].GetReplicationFactor(),
            statistics.regular_disk_space(),
            statistics.erasure_disk_space());
    }
    result.ChunkCount = statistics.chunk_count();
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
