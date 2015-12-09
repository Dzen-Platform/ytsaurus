#include "chunk_owner_base.h"
#include "chunk_list.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/cluster_resources.h>

#include <yt/ytlib/chunk_client/data_statistics.h>

namespace NYT {
namespace NChunkServer {

using namespace NYTree;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressClient;
using namespace NCypressClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TChunkOwnerBase::TChunkOwnerBase(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
    , ChunkList_(nullptr)
    , UpdateMode_(EUpdateMode::None)
    , ReplicationFactor_(0)
    , Vital_(true)
    , ChunkPropertiesUpdateNeeded_(false)
{ }

void TChunkOwnerBase::Save(NCellMaster::TSaveContext& context) const
{
    TCypressNodeBase::Save(context);

    using NYT::Save;
    Save(context, ChunkList_);
    Save(context, UpdateMode_);
    Save(context, ReplicationFactor_);
    Save(context, Vital_);
    Save(context, ChunkPropertiesUpdateNeeded_);
    Save(context, SnapshotStatistics_);
    Save(context, DeltaStatistics_);
}

void TChunkOwnerBase::Load(NCellMaster::TLoadContext& context)
{
    TCypressNodeBase::Load(context);

    using NYT::Load;
    Load(context, ChunkList_);
    Load(context, UpdateMode_);
    Load(context, ReplicationFactor_);
    Load(context, Vital_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 203) {
        Load(context, ChunkPropertiesUpdateNeeded_);
    }
    // COMPAT(babenko)
    if (context.GetVersion() >= 200) {
        Load(context, SnapshotStatistics_);
        Load(context, DeltaStatistics_);
    }
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
            YUNREACHABLE();
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
            YUNREACHABLE();
    }
}

void TChunkOwnerBase::BeginUpload(EUpdateMode mode)
{
    UpdateMode_ = mode;
}

void TChunkOwnerBase::EndUpload(
    const TDataStatistics* statistics,
    bool deriveStatistics,
    const std::vector<Stroka>& /*keyColumns*/)
{
    TNullable<TDataStatistics> updateStatistics;

    if (!IsExternal()) {
        updateStatistics = ComputeUpdateStatistics();
    }

    if (deriveStatistics) {
        statistics = &*updateStatistics;
    } else if (statistics && updateStatistics) {
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
                YUNREACHABLE();
        }
    }
}

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
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
