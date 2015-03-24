#include "stdafx.h"
#include "private.h"
#include "config.h"
#include "location.h"
#include "blob_chunk.h"
#include "journal_chunk.h"
#include "chunk_store.h"
#include "master_connector.h"
#include "session_manager.h"
#include "session.h"

#include <core/misc/fs.h>

#include <ytlib/chunk_client/data_node_service_proxy.h>

#include <ytlib/object_client/helpers.h>

#include <server/cell_node/bootstrap.h>

#include <util/random/random.h>

#include <utility>
#include <limits>

namespace NYT {
namespace NDataNode {

using namespace NObjectClient;
using namespace NChunkClient;
using namespace NCellNode;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkStore::TChunkStore(TDataNodeConfigPtr config, TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetControlInvoker(), ControlThread);
}

void TChunkStore::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    for (int i = 0; i < Config_->StoreLocations.size(); ++i) {
        auto locationConfig = Config_->StoreLocations[i];

        auto location = New<TLocation>(
            ELocationType::Store,
            "store" + ToString(i),
            locationConfig,
            Bootstrap_);

        location->SubscribeDisabled(
            BIND(&TChunkStore::OnLocationDisabled, Unretained(this), location)
                .Via(Bootstrap_->GetControlInvoker()));
            
        auto descriptors = location->Initialize();
        for (const auto& descriptor : descriptors) {
            auto chunk = CreateFromDescriptor(location, descriptor);
            RegisterExistingChunk(chunk);
        }

        Locations_.push_back(location);
    }

    LOG_INFO("Chunk store initialized, %v chunks total",
        GetChunkCount());
}

void TChunkStore::RegisterNewChunk(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // NB: The location was surely enabled the moment the chunk was created
    // but it may have got disabled later.
    auto location = chunk->GetLocation();
    if (!location->IsEnabled())
        return;

    auto entry = BuildEntry(chunk);
    auto result = ChunkMap_.insert(std::make_pair(chunk->GetId(), entry));
    if (!result.second) {
        auto oldChunk = result.first->second.Chunk;
        LOG_FATAL("Duplicate chunk: %v vs %v",
            chunk->GetLocation()->GetChunkPath(chunk->GetId()),
            oldChunk->GetLocation()->GetChunkPath(oldChunk->GetId()));
    }

    DoRegisterChunk(entry);
}

void TChunkStore::RegisterExistingChunk(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(chunk->GetLocation()->IsEnabled());

    bool doRegister = true;
    auto it = ChunkMap_.find(chunk->GetId());
    if (it != ChunkMap_.end()) {
        auto oldChunk = it->second.Chunk;
        auto oldPath = oldChunk->GetLocation()->GetChunkPath(oldChunk->GetId());
        auto currentPath = chunk->GetLocation()->GetChunkPath(chunk->GetId());

        // Check that replicas point to the different inodes.
        LOG_FATAL_IF(
            NFS::AreInodesIdentical(oldPath, currentPath),
            "Duplicate chunks point to the same inode: %v vs %v",
            currentPath,
            oldPath);

        switch (TypeFromId(DecodeChunkId(chunk->GetId()).Id)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk: {
                // Compare if replicas are equal.
                LOG_FATAL_IF(
                    oldChunk->GetInfo().disk_space() != chunk->GetInfo().disk_space(),
                    "Duplicate chunks with different size: %v vs %v",
                    currentPath,
                    oldPath);

                // Remove duplicate replica.
                LOG_WARNING("Removing duplicate blob chunk: %v vs %v",
                    currentPath,
                    oldPath);
                chunk->SyncRemove(true);
                doRegister = false;
                break;
            }

            case EObjectType::JournalChunk: {
                auto longerRowCount = chunk->AsJournalChunk()->GetRowCount();
                auto shorterRowCount = oldChunk->AsJournalChunk()->GetRowCount();

                auto longerChunk = chunk;
                auto shorterChunk = oldChunk;

                if (longerRowCount < shorterRowCount) {
                    std::swap(longerRowCount, shorterRowCount);
                    std::swap(longerChunk, shorterChunk);
                }

                // Remove shorter replica.
                LOG_WARNING("Removing shorter journal chunk: %v (%v rows) vs %v (%v rows)",
                    shorterChunk->GetFileName(),
                    shorterRowCount,
                    longerChunk->GetFileName(),
                    longerRowCount);
                shorterChunk->SyncRemove(true);
                if (shorterChunk == oldChunk) {
                    UnregisterChunk(oldChunk);
                } else {
                    doRegister = false;
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    if (doRegister) {
        auto entry = BuildEntry(chunk);
        YCHECK(ChunkMap_.insert(std::make_pair(chunk->GetId(), entry)).second);
        DoRegisterChunk(entry);
    }
}

void TChunkStore::DoRegisterChunk(const TChunkEntry& entry)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto chunk = entry.Chunk;
    auto location = chunk->GetLocation();
    location->UpdateChunkCount(+1);
    location->UpdateUsedSpace(+entry.DiskSpace);

    switch (TypeFromId(DecodeChunkId(chunk->GetId()).Id)) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            LOG_DEBUG("Blob chunk registered (ChunkId: %v, DiskSpace: %v)",
                chunk->GetId(),
                entry.DiskSpace);
            break;

        case EObjectType::JournalChunk:
            LOG_DEBUG("Journal chunk registered (ChunkId: %v, Version: %v, Sealed: %v, Active: %v)",
                chunk->GetId(),
                chunk->GetVersion(),
                chunk->GetInfo().sealed(),
                chunk->IsActive());
            break;

        default:
            YUNREACHABLE();
    }

    ChunkAdded_.Fire(chunk);
}

void TChunkStore::UpdateExistingChunk(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto location = chunk->GetLocation();
    if (!location->IsEnabled())
        return;

    chunk->IncrementVersion();

    auto it = ChunkMap_.find(chunk->GetId());
    YCHECK(it != ChunkMap_.end());
    auto& entry = it->second;

    location->UpdateUsedSpace(-entry.DiskSpace);

    entry = BuildEntry(chunk);

    location->UpdateUsedSpace(+entry.DiskSpace);

    switch (TypeFromId(DecodeChunkId(chunk->GetId()).Id)) {
        case EObjectType::JournalChunk:
            LOG_DEBUG("Journal chunk updated (ChunkId: %v, Version: %v, Sealed: %v, Active: %v)",
                chunk->GetId(),
                chunk->GetVersion(),
                chunk->GetInfo().sealed(),
                chunk->IsActive());
            break;

        default:
            YUNREACHABLE();
    }

    ChunkAdded_.Fire(chunk);
}

void TChunkStore::UnregisterChunk(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto location = chunk->GetLocation();
    if (!location->IsEnabled())
        return;

    auto it = ChunkMap_.find(chunk->GetId());
    // NB: Concurrent chunk removals are possible.
    if (it == ChunkMap_.end())
        return;

    const auto& entry = it->second;

    location->UpdateChunkCount(-1);
    location->UpdateUsedSpace(-entry.DiskSpace);

    ChunkMap_.erase(it);

    LOG_DEBUG("Chunk unregistered (ChunkId: %v)",
        chunk->GetId());

    ChunkRemoved_.Fire(chunk);
}

TChunkStore::TChunkEntry TChunkStore::BuildEntry(IChunkPtr chunk)
{
    TChunkEntry result;
    result.Chunk = chunk;
    result.DiskSpace = chunk->GetInfo().disk_space();
    return result;
}

IChunkPtr TChunkStore::FindChunk(const TChunkId& chunkId) const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto it = ChunkMap_.find(chunkId);
    return it == ChunkMap_.end() ? nullptr : it->second.Chunk;
}

IChunkPtr TChunkStore::GetChunkOrThrow(const TChunkId& chunkId) const
{
    auto chunk = FindChunk(chunkId);
    if (!chunk) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::NoSuchChunk,
            "No such chunk %v",
            chunkId);
    }
    return chunk;
}

TFuture<void> TChunkStore::RemoveChunk(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return chunk->ScheduleRemove().Apply(
        BIND(&TChunkStore::UnregisterChunk, MakeStrong(this), chunk)
            .Via(Bootstrap_->GetControlInvoker()));
}

TLocationPtr TChunkStore::GetNewChunkLocation()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    std::vector<TLocationPtr> candidates;
    int minCount = std::numeric_limits<int>::max();
    for (const auto& location : Locations_) {
        if (location->IsFull() || !location->IsEnabled()) {
            continue;
        }
        int count = location->GetSessionCount();
        if (count < minCount) {
            candidates.clear();
            minCount = count;
        }
        if (count == minCount) {
            candidates.push_back(location);
        }
    }

    if (candidates.empty()) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::OutOfSpace,
            "All locations are either disabled or full");
    }

    return candidates[RandomNumber(candidates.size())];
}

TLocationPtr TChunkStore::GetReplayedChunkLocation()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    TLocationPtr candidate;
    for (const auto& location : Locations_) {
        if (!location->IsEnabled()) {
            continue;
        }
        if (!candidate || location->GetAvailableSpace() > candidate->GetAvailableSpace()) {
            candidate = location;
        }
    }

    if (!candidate) {
        THROW_ERROR_EXCEPTION("All locations are disabled");
    }

    return candidate;
}

TChunkStore::TChunks TChunkStore::GetChunks() const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    TChunks result;
    result.reserve(ChunkMap_.size());
    for (const auto& pair : ChunkMap_) {
        result.push_back(pair.second.Chunk);
    }
    return result;
}

int TChunkStore::GetChunkCount() const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return static_cast<int>(ChunkMap_.size());
}

void TChunkStore::OnLocationDisabled(TLocationPtr location, const TError& reason)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // Scan through all chunks and remove those residing on this dead location.
    {
        int count = 0;
        auto it = ChunkMap_.begin();
        while (it != ChunkMap_.end()) {
            auto jt = it++;
            auto chunk = jt->second.Chunk;
            if (chunk->GetLocation() == location) {
                ChunkMap_.erase(jt);
                ++count;
            }
        }
        LOG_INFO("%v chunks discarded at disabled location (LocationId: %v)",
            count,
            location->GetId());
    }

    // Scan through all sessions and cancel those opened for discarded chunks.
    {
        int count = 0;
        auto sessionManager = Bootstrap_->GetSessionManager();
        auto sessions = sessionManager->GetSessions();
        for (auto session : sessions) {
            if (session->GetLocation() == location) {
                session->Cancel(TError("Location disabled"));
                ++count;
            }
        }
        LOG_INFO("%v sessions canceled at disabled location (LocationId: %v)",
            count,
            location->GetId());
    }

    // Register an alert and
    // schedule an out-of-order heartbeat to notify the master about the disaster.
    auto masterConnector = Bootstrap_->GetMasterConnector();
    masterConnector->RegisterAlert(Format("Chunk store at %v is disabled\n%v",
        location->GetPath(),
        reason));
    masterConnector->ForceRegister();
}

IChunkPtr TChunkStore::CreateFromDescriptor(
    TLocationPtr location,
    const TChunkDescriptor& descriptor)
{
    auto chunkType = TypeFromId(DecodeChunkId(descriptor.Id).Id);
    switch (chunkType) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            if (location->GetType() == ELocationType::Store) {
                return New<TStoredBlobChunk>(
                    Bootstrap_,
                    location,
                    descriptor);
            } else {
                return New<TCachedBlobChunk>(
                    Bootstrap_,
                    location,
                    descriptor);
            }

        case EObjectType::JournalChunk:
            return New<TJournalChunk>(
                Bootstrap_,
                location,
                descriptor);

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
