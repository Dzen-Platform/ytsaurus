#include "stdafx.h"
#include "session_manager.h"
#include "blob_session.h"
#include "journal_session.h"
#include "private.h"
#include "config.h"
#include "location.h"
#include "block_store.h"
#include "blob_chunk.h"
#include "chunk_store.h"

#include <core/misc/fs.h>

#include <core/profiling/profile_manager.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/chunk_client/chunk_replica.h>
#include <ytlib/chunk_client/file_writer.h>
#include <ytlib/chunk_client/chunk_meta.pb.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <core/profiling/scoped_timer.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NDataNode {

using namespace NRpc;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NCellNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;
static const auto& Profiler = DataNodeProfiler;

////////////////////////////////////////////////////////////////////////////////

TSessionManager::TSessionManager(
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
{
    YCHECK(config);
    YCHECK(bootstrap);
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetControlInvoker(), ControlThread);

    auto* profilingManager = NProfiling::TProfileManager::Get();
    for (auto type : TEnumTraits<ESessionType>::GetDomainValues()) {
        PerTypeSessionCounters_[type] = NProfiling::TSimpleCounter(
            "/session_count",
            {profilingManager->RegisterTag("type", type)});
    }
}

ISessionPtr TSessionManager::FindSession(const TChunkId& chunkId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto it = SessionMap_.find(chunkId);
    return it == SessionMap_.end() ? nullptr : it->second;
}

ISessionPtr TSessionManager::GetSession(const TChunkId& chunkId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto session = FindSession(chunkId);
    if (!session) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::NoSuchSession,
            "Session %v is invalid or expired",
            chunkId);
    }
    return session;
}

ISessionPtr TSessionManager::StartSession(
    const TChunkId& chunkId,
    const TSessionOptions& options)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (SessionMap_.size() >= Config_->MaxWriteSessions) {
        auto error = TError("Maximum concurrent write session limit %v has been reached",
            Config_->MaxWriteSessions);
        LOG_ERROR(error);
        THROW_ERROR(error);
    }
    
    auto session = CreateSession(chunkId, options);

    session->SubscribeFinished(
        BIND(&TSessionManager::OnSessionFinished, MakeStrong(this), Unretained(session.Get()))
            .Via(Bootstrap_->GetControlInvoker()));

    RegisterSession(session);

    return session;
}

ISessionPtr TSessionManager::CreateSession(
    const TChunkId& chunkId,
    const TSessionOptions& options)
{
    auto chunkType = TypeFromId(DecodeChunkId(chunkId).Id);

    auto chunkStore = Bootstrap_->GetChunkStore();
    auto location = chunkStore->GetNewChunkLocation(chunkType);

    auto lease = TLeaseManager::CreateLease(
        Config_->SessionTimeout,
        BIND(&TSessionManager::OnSessionLeaseExpired, MakeStrong(this), chunkId)
            .Via(Bootstrap_->GetControlInvoker()));

    ISessionPtr session;
    switch (chunkType) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            session = New<TBlobSession>(
                Config_,
                Bootstrap_,
                chunkId,
                options,
                location,
                lease);
            break;

        case EObjectType::JournalChunk:
            session = New<TJournalSession>(
                Config_,
                Bootstrap_,
                chunkId,
                options,
                location,
                lease);
            break;
    
        default:
            THROW_ERROR_EXCEPTION("Invalid session chunk type %Qlv",
                chunkType);
    }

    return session;
}

void TSessionManager::OnSessionLeaseExpired(const TChunkId& chunkId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto session = FindSession(chunkId);
    if (!session)
        return;

    LOG_INFO("Session lease expired (ChunkId: %v)",
        chunkId);

    session->Cancel(TError("Session lease expired"));
}

void TSessionManager::OnSessionFinished(ISession* session, const TError& /*error*/)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Session finished (ChunkId: %v)",
        session->GetChunkId());

    UnregisterSession(session);
}

int TSessionManager::GetSessionCount(ESessionType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return PerTypeSessionCounters_[type].Current.load();
}

void TSessionManager::RegisterSession(ISessionPtr session)
{
    Profiler.Increment(PerTypeSessionCounters_[session->GetType()], +1);
    YCHECK(SessionMap_.insert(std::make_pair(session->GetChunkId(), session)).second);
}

void TSessionManager::UnregisterSession(ISessionPtr session)
{
    Profiler.Increment(PerTypeSessionCounters_[session->GetType()], -1);
    YCHECK(SessionMap_.erase(session->GetChunkId()) == 1);
}

std::vector<ISessionPtr> TSessionManager::GetSessions()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    std::vector<ISessionPtr> result;
    for (const auto& pair : SessionMap_) {
        result.push_back(pair.second);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
