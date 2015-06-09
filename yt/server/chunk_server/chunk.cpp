#include "stdafx.h"
#include "chunk.h"
#include "chunk_tree_statistics.h"
#include "chunk_list.h"

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/object_client/helpers.h>

#include <core/erasure/codec.h>

#include <server/cell_master/serialize.h>

namespace NYT {
namespace NChunkServer {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NSecurityServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

bool operator== (const TChunkProperties& lhs, const TChunkProperties& rhs)
{
    return
        lhs.ReplicationFactor == rhs.ReplicationFactor &&
        lhs.Vital == rhs.Vital;
}

bool operator!= (const TChunkProperties& lhs, const TChunkProperties& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

TChunk::TChunk(const TChunkId& id)
    : TChunkTree(id)
    , Flags_{}
    , ReplicationFactor_(1)
    , ReadQuorum_(0)
    , WriteQuorum_(0)
    , ErasureCodec_(NErasure::ECodec::None)
{
    ChunkMeta_.set_type(static_cast<int>(EChunkType::Unknown));
    ChunkMeta_.set_version(-1);
    ChunkMeta_.mutable_extensions();
}

TChunkDynamicData* TChunk::GetDynamicData() const
{
    return GetTypedDynamicData<TChunkDynamicData>();
}

TChunkTreeStatistics TChunk::GetStatistics() const
{
    YASSERT(IsConfirmed());

    TChunkTreeStatistics result;
    result.RowCount = MiscExt_.row_count();
    result.UncompressedDataSize = MiscExt_.uncompressed_data_size();
    result.CompressedDataSize = MiscExt_.compressed_data_size();
    result.DataWeight = MiscExt_.data_weight();

    if (IsErasure()) {
        result.ErasureDiskSpace = ChunkInfo_.disk_space();
    } else {
        result.RegularDiskSpace = ChunkInfo_.disk_space();
    }

    result.ChunkCount = 1;
    result.Rank = 0;
    result.Sealed = IsSealed();

    return result;
}

TClusterResources TChunk::GetResourceUsage() const
{
    i64 diskSpace = IsConfirmed() ? ChunkInfo_.disk_space() * GetReplicationFactor() : 0;
    return TClusterResources(diskSpace, 0, 1);
}

void TChunk::Save(NCellMaster::TSaveContext& context) const
{
    TChunkTree::Save(context);

    using NYT::Save;
    Save(context, ChunkInfo_);
    Save(context, ChunkMeta_);
    Save(context, ReplicationFactor_);
    Save(context, ReadQuorum_);
    Save(context, WriteQuorum_);
    Save(context, GetErasureCodec());
    Save(context, GetMovable());
    Save(context, GetVital());
    Save(context, Parents_);
    // NB: RemoveReplica calls do not commute and their order is not
    // deterministic (i.e. when unregistering a node we traverse certain hashtables).
    TVectorSerializer<TDefaultSerializer, TSortedTag>::Save(context, StoredReplicas_);
    Save(context, CachedReplicas_);
}

void TChunk::Load(NCellMaster::TLoadContext& context)
{
    TChunkTree::Load(context);

    using NYT::Load;
    Load(context, ChunkInfo_);
    Load(context, ChunkMeta_);

    // COMPAT(babenko)
    if (context.GetVersion() < 100) {
        SetReplicationFactor(Load<i16>(context));
    } else {
        SetReplicationFactor(Load<i8>(context));
        SetReadQuorum(Load<i8>(context));
        SetWriteQuorum(Load<i8>(context));
    }
    // COMPAT(babenko)
    if (context.GetVersion() < 111) {
        SetErasureCodec(NErasure::ECodec(Load<int>(context)));
    } else {
        SetErasureCodec(Load<NErasure::ECodec>(context));
    }

    SetMovable(Load<bool>(context));
    SetVital(Load<bool>(context));

    Load(context, Parents_);
    Load(context, StoredReplicas_);
    Load(context, CachedReplicas_);
    
    if (IsConfirmed()) {
        MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
    }
}

void TChunk::AddReplica(TNodePtrWithIndex replica, bool cached)
{
    if (cached) {
        YASSERT(!IsJournal());
        if (!CachedReplicas_) {
            CachedReplicas_.reset(new yhash_set<TNodePtrWithIndex>());
        }
        YCHECK(CachedReplicas_->insert(replica).second);
    } else {
        if (IsJournal()) {
            for (auto& existingReplica : StoredReplicas_) {
                if (existingReplica.GetPtr() == replica.GetPtr()) {
                    existingReplica = replica;
                    return;
                }
            }
        }
        StoredReplicas_.push_back(replica);
    }
}

void TChunk::RemoveReplica(TNodePtrWithIndex replica, bool cached)
{
    if (cached) {
        YASSERT(CachedReplicas_);
        YCHECK(CachedReplicas_->erase(replica) == 1);
        if (CachedReplicas_->empty()) {
            CachedReplicas_.reset();
        }
    } else {
        for (auto it = StoredReplicas_.begin(); it != StoredReplicas_.end(); ++it) {
            auto& existingReplica = *it;
            if (existingReplica == replica ||
                IsJournal() && existingReplica.GetPtr() == replica.GetPtr())
            {
                std::swap(existingReplica, StoredReplicas_.back());
                StoredReplicas_.resize(StoredReplicas_.size() - 1);
                return;
            }
        }
        YUNREACHABLE();
    }
}

TNodePtrWithIndexList TChunk::GetReplicas() const
{
    TNodePtrWithIndexList result(StoredReplicas_.begin(), StoredReplicas_.end());
    if (CachedReplicas_) {
        result.insert(result.end(), CachedReplicas_->begin(), CachedReplicas_->end());
    }
    return result;
}

void TChunk::ApproveReplica(TNodePtrWithIndex replica)
{
    if (IsJournal()) {
        for (auto& existingReplica : StoredReplicas_) {
            if (existingReplica.GetPtr() == replica.GetPtr()) {
                existingReplica = replica;
                return;
            }
        }
        YUNREACHABLE();
    }
}

void TChunk::Confirm(
    TChunkInfo* chunkInfo,
    TChunkMeta* chunkMeta)
{
    ChunkInfo_.Swap(chunkInfo);
    ChunkMeta_.Swap(chunkMeta);
    MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());

    YASSERT(IsConfirmed());
}

bool TChunk::IsConfirmed() const
{
    return EChunkType(ChunkMeta_.type()) != EChunkType::Unknown;
}

void TChunk::ValidateConfirmed()
{
    if (!IsConfirmed()) {
        THROW_ERROR_EXCEPTION("Chunk %v is not confirmed",
            Id);
    }
}

bool TChunk::GetMovable() const
{
    return Flags_.Movable;
}

void TChunk::SetMovable(bool value)
{
    Flags_.Movable = value;
}

bool TChunk::GetVital() const
{
    return Flags_.Vital;
}

void TChunk::SetVital(bool value)
{
    Flags_.Vital = value;
}

bool TChunk::GetRefreshScheduled() const
{
    return GetDynamicData()->Flags.RefreshScheduled;
}

void TChunk::SetRefreshScheduled(bool value)
{
    GetDynamicData()->Flags.RefreshScheduled = value;
}

bool TChunk::GetPropertiesUpdateScheduled() const
{
    return GetDynamicData()->Flags.PropertiesUpdateScheduled;
}

void TChunk::SetPropertiesUpdateScheduled(bool value)
{
    GetDynamicData()->Flags.PropertiesUpdateScheduled = value;
}

bool TChunk::GetSealScheduled() const
{
    return GetDynamicData()->Flags.SealScheduled;
}

void TChunk::SetSealScheduled(bool value)
{
    GetDynamicData()->Flags.SealScheduled = value;
}

const TNullable<TChunkRepairQueueIterator>& TChunk::GetRepairQueueIterator() const
{
    return GetDynamicData()->RepairQueueIterator;
}

void TChunk::SetRepairQueueIterator(const TNullable<TChunkRepairQueueIterator>& value)
{
    GetDynamicData()->RepairQueueIterator = value;
}

int TChunk::GetReplicationFactor() const
{
    return ReplicationFactor_;
}

void TChunk::SetReplicationFactor(int value)
{
    ReplicationFactor_ = value;
}

int TChunk::GetReadQuorum() const
{
    return ReadQuorum_;
}

void TChunk::SetReadQuorum(int value)
{
    ReadQuorum_ = value;
}

int TChunk::GetWriteQuorum() const
{
    return WriteQuorum_;
}

void TChunk::SetWriteQuorum(int value)
{
    WriteQuorum_ = value;
}

NErasure::ECodec TChunk::GetErasureCodec() const
{
    return ErasureCodec_;
}

void TChunk::SetErasureCodec(NErasure::ECodec value)
{
    ErasureCodec_ = value;
}

bool TChunk::IsErasure() const
{
    return GetType() == EObjectType::ErasureChunk;
}

bool TChunk::IsJournal() const
{
    return GetType() == EObjectType::JournalChunk;
}

bool TChunk::IsRegular() const
{
    return GetType() == EObjectType::Chunk;
}

bool TChunk::IsAvailable() const
{
    if (IsRegular()) {
        return !StoredReplicas_.empty();
    } else if (IsErasure()) {
        auto* codec = NErasure::GetCodec(GetErasureCodec());
        int dataPartCount = codec->GetDataPartCount();
        NErasure::TPartIndexSet missingIndexSet((1 << dataPartCount) - 1);
        for (auto replica : StoredReplicas_) {
            missingIndexSet.reset(replica.GetIndex());
        }
        return !missingIndexSet.any();
    } else if (IsJournal()) {
        if (StoredReplicas_.size() >= GetReadQuorum()) {
            return true;
        }
        for (auto replica : StoredReplicas_) {
            if (replica.GetIndex() == SealedChunkReplicaIndex) {
                return true;
            }
        }
        return false;
    } else {
        YUNREACHABLE();
    }
}

bool TChunk::IsSealed() const
{
    if (!IsConfirmed()) {
        return false;
    }

    if (!IsJournal()) {
        return true;
    }

    return MiscExt_.sealed();
}

i64 TChunk::GetSealedRowCount() const
{
    YCHECK(MiscExt_.sealed());
    return MiscExt_.row_count();
}

void TChunk::Seal(const TMiscExt& info)
{
    YASSERT(IsConfirmed());

    // NB: Just a sanity check.
    YCHECK(!MiscExt_.sealed());
    YCHECK(MiscExt_.row_count() == 0);
    YCHECK(MiscExt_.uncompressed_data_size() == 0);
    YCHECK(MiscExt_.compressed_data_size() == 0);
    YCHECK(ChunkInfo_.disk_space() == 0);

    MiscExt_.set_sealed(true);
    MiscExt_.set_row_count(info.row_count());
    MiscExt_.set_uncompressed_data_size(info.uncompressed_data_size());
    MiscExt_.set_compressed_data_size(info.compressed_data_size());
    SetProtoExtension(ChunkMeta_.mutable_extensions(), MiscExt_);
    ChunkInfo_.set_disk_space(info.uncompressed_data_size());  // an approximation
}

TChunkProperties TChunk::GetChunkProperties() const
{
    TChunkProperties result;
    result.ReplicationFactor = GetReplicationFactor();
    result.Vital = GetVital();
    return result;
}

int TChunk::GetMaxReplicasPerRack(TNullable<int> replicationFactorOverride) const
{
    int replicationFactor = replicationFactorOverride.Get(GetReplicationFactor());
    switch (GetType()) {
        case EObjectType::Chunk:
            return std::max(replicationFactor - 1, 1);

        case EObjectType::ErasureChunk:
            return NErasure::GetCodec(GetErasureCodec())->GetGuaranteedRepairablePartCount();

        case EObjectType::JournalChunk:
            return 1;

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
