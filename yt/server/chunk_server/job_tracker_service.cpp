#include "job_tracker_service.h"
#include "private.h"
#include "chunk.h"
#include "chunk_manager.h"
#include "job.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/master_hydra_service.h>

#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_directory_builder.h>
#include <yt/server/node_tracker_server/node_tracker.h>

#include <yt/ytlib/chunk_client/job.pb.h>

#include <yt/ytlib/job_tracker_client/job_tracker_service_proxy.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/string.h>

#include <yt/core/rpc/helpers.h>

namespace NYT {
namespace NChunkServer {

using namespace NHydra;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer;
using namespace NJobTrackerClient::NProto;
using namespace NChunkClient::NProto;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TJobTrackerService
    : public NCellMaster::TMasterHydraServiceBase
{
public:
    explicit TJobTrackerService(TBootstrap* bootstrap)
        : TMasterHydraServiceBase(
            bootstrap,
            TJobTrackerServiceProxy::GetServiceName(),
            ChunkServerLogger,
            TJobTrackerServiceProxy::GetProtocolVersion())
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
    }

private:
    DECLARE_RPC_SERVICE_METHOD(NJobTrackerClient::NProto, Heartbeat)
    {
        ValidatePeer(EPeerKind::Leader);

        auto nodeId = request->node_id();

        const auto& resourceLimits = request->resource_limits();
        const auto& resourceUsage = request->resource_usage();

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        auto* node = nodeTracker->GetNodeOrThrow(nodeId);

        context->SetRequestInfo("NodeId: %v, Address: %v, ResourceUsage: {%v}",
            nodeId,
            node->GetDefaultAddress(),
            FormatResourceUsage(resourceUsage, resourceLimits));

        if (node->GetLocalState() != ENodeState::Online) {
            THROW_ERROR_EXCEPTION(
                NNodeTrackerClient::EErrorCode::InvalidState,
                "Cannot process a heartbeat in %Qlv state",
                node->GetLocalState());
        }

        node->ResourceLimits() = resourceLimits;
        node->ResourceUsage() = resourceUsage;

        auto chunkManager = Bootstrap_->GetChunkManager();
        std::vector<TJobPtr> currentJobs;
        for (const auto& jobStatus : request->jobs()) {
            auto jobId = FromProto<TJobId>(jobStatus.job_id());
            auto state = EJobState(jobStatus.state());
            auto job = chunkManager->FindJob(jobId);
            if (job) {
                job->SetState(state);
                if (state == EJobState::Completed || state == EJobState::Failed) {
                    job->Error() = FromProto<TError>(jobStatus.result().error());
                }
                currentJobs.push_back(job);
            } else {
                switch (state) {
                    case EJobState::Completed:
                        LOG_WARNING("Unknown job has completed, removal scheduled (JobId: %v)",
                            jobId);
                        ToProto(response->add_jobs_to_remove(), jobId);
                        break;

                    case EJobState::Failed:
                        LOG_INFO("Unknown job has failed, removal scheduled (JobId: %v)",
                            jobId);
                        ToProto(response->add_jobs_to_remove(), jobId);
                        break;

                    case EJobState::Aborted:
                        LOG_INFO("Job aborted, removal scheduled (JobId: %v)",
                            jobId);
                        ToProto(response->add_jobs_to_remove(), jobId);
                        break;

                    case EJobState::Running:
                        LOG_WARNING("Unknown job is running, abort scheduled (JobId: %v)",
                            jobId);
                        ToProto(response->add_jobs_to_abort(), jobId);
                        break;

                    case EJobState::Waiting:
                        LOG_WARNING("Unknown job is waiting, abort scheduled (JobId: %v)",
                            jobId);
                        ToProto(response->add_jobs_to_abort(), jobId);
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
        }

        std::vector<TJobPtr> jobsToStart;
        std::vector<TJobPtr> jobsToAbort;
        std::vector<TJobPtr> jobsToRemove;
        chunkManager->ScheduleJobs(
            node,
            currentJobs,
            &jobsToStart,
            &jobsToAbort,
            &jobsToRemove);

        for (auto job : jobsToStart) {
            const auto& chunkIdWithIndex = job->GetChunkIdWithIndex();

            auto* jobInfo = response->add_jobs_to_start();
            ToProto(jobInfo->mutable_job_id(), job->GetJobId());
            *jobInfo->mutable_resource_limits() = job->ResourceUsage();

            auto* jobSpec = jobInfo->mutable_spec();
            jobSpec->set_type(static_cast<int>(job->GetType()));

            auto* chunkjobSpecExt = jobSpec->MutableExtension(TChunkJobSpecExt::chunk_job_spec_ext);
            ToProto(chunkjobSpecExt->mutable_chunk_id(), EncodeChunkId(chunkIdWithIndex));

            switch (job->GetType()) {
                case EJobType::ReplicateChunk: {
                    auto* replciateChunkJobSpecExt = jobSpec->MutableExtension(TReplicateChunkJobSpecExt::replicate_chunk_job_spec_ext);

                    auto targetReplicas = AddressesToReplicas(job->TargetAddresses());
                    ToProto(replciateChunkJobSpecExt->mutable_targets(), targetReplicas);

                    NNodeTrackerServer::TNodeDirectoryBuilder builder(replciateChunkJobSpecExt->mutable_node_directory());
                    builder.Add(targetReplicas);
                    break;
                }

                case EJobType::RemoveChunk:
                    break;

                case EJobType::RepairChunk: {
                    auto* chunk = chunkManager->GetChunk(chunkIdWithIndex.Id);

                    auto* repairChunkJobSpecExt = jobSpec->MutableExtension(TRepairChunkJobSpecExt::repair_chunk_job_spec_ext);
                    repairChunkJobSpecExt->set_erasure_codec(static_cast<int>(chunk->GetErasureCodec()));
                    ToProto(repairChunkJobSpecExt->mutable_erased_indexes(), job->ErasedIndexes());

                    NNodeTrackerServer::TNodeDirectoryBuilder builder(repairChunkJobSpecExt->mutable_node_directory());
                    const auto& replicas = chunk->StoredReplicas();
                    builder.Add(replicas);
                    ToProto(repairChunkJobSpecExt->mutable_replicas(), replicas);

                    auto targetReplicas = AddressesToReplicas(job->TargetAddresses());
                    builder.Add(targetReplicas);
                    ToProto(repairChunkJobSpecExt->mutable_targets(), targetReplicas);
                    break;
                }

                case EJobType::SealChunk: {
                    auto* chunk = chunkManager->GetChunk(chunkIdWithIndex.Id);

                    auto* sealChunkJobSpecExt = jobSpec->MutableExtension(TSealChunkJobSpecExt::seal_chunk_job_spec_ext);

                    sealChunkJobSpecExt->set_row_count(chunk->GetSealedRowCount());

                    NNodeTrackerServer::TNodeDirectoryBuilder builder(sealChunkJobSpecExt->mutable_node_directory());
                    const auto& replicas = chunk->StoredReplicas();
                    builder.Add(replicas);
                    ToProto(sealChunkJobSpecExt->mutable_replicas(), replicas);
                    break;
                }

                default:
                    YUNREACHABLE();
            }
        }

        for (auto job : jobsToAbort) {
            ToProto(response->add_jobs_to_abort(), job->GetJobId());
        }

        for (auto job : jobsToRemove) {
            ToProto(response->add_jobs_to_remove(), job->GetJobId());
        }

        context->Reply();
    }

    TNodePtrWithIndexList AddressesToReplicas(const std::vector<Stroka>& addresses)
    {
        TNodePtrWithIndexList replicas;
        auto nodeTracker = Bootstrap_->GetNodeTracker();
        for (const auto& address : addresses) {
            auto* node = nodeTracker->GetNodeByAddress(address);
            TNodePtrWithIndex replica(node, 0);
            replicas.push_back(replica);
        }
        return replicas;
    }

};

NRpc::IServicePtr CreateJobTrackerService(TBootstrap* boostrap)
{
    return New<TJobTrackerService>(boostrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
