#include "chaos_cell_directory_synchronizer.h"

#include "config.h"

#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/chaos_client/chaos_master_service_proxy.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/rpc/dispatcher.h>

namespace NYT::NChaosClient {

using namespace NApi::NNative;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NObjectClient;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TChaosCellDirectorySynchronizer
    : public IChaosCellDirectorySynchronizer
{
public:
    TChaosCellDirectorySynchronizer(
        TChaosCellDirectorySynchronizerConfigPtr config,
        ICellDirectoryPtr cellDirectory,
        IConnectionPtr connection,
        NLogging::TLogger logger)
        : Config_(std::move(config))
        , CellDirectory_(std::move(cellDirectory))
        , Connection_(std::move(connection))
        , Logger(std::move(logger))
        , SyncExecutor_(New<TPeriodicExecutor>(
            NRpc::TDispatcher::Get()->GetHeavyInvoker(),
            BIND(&TChaosCellDirectorySynchronizer::OnSync, MakeWeak(this)),
            TPeriodicExecutorOptions{
                .Period = Config_->SyncPeriod,
                .Splay = Config_->SyncPeriodSplay
            }))
    { }

    void AddCellIds(const std::vector<NObjectClient::TCellId>& cellIds) override
    {
        auto guard = Guard(SpinLock_);

        for (auto cellId : cellIds) {
            AddCell(CellTagFromId(cellId), cellId);
        }
    }

    void AddCellTag(TCellTag cellTag) override
    {
        auto guard = Guard(SpinLock_);
        AddCell(cellTag);
    }

    void Start() override
    {
        auto guard = Guard(SpinLock_);
        DoStart();
    }

    void Stop() override
    {
        auto guard = Guard(SpinLock_);
        DoStop();
    }

    TFuture<void> Sync() override
    {
        auto guard = Guard(SpinLock_);
        if (Stopped_) {
            return MakeFuture(TError("Chaos cell directory synchronizer is stopped"));
        }
        DoStart();
        return SyncPromise_.ToFuture();
    }

private:
    const TChaosCellDirectorySynchronizerConfigPtr Config_;
    const ICellDirectoryPtr CellDirectory_;
    const TWeakPtr<IConnection> Connection_;

    const NLogging::TLogger Logger;
    const TPeriodicExecutorPtr SyncExecutor_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);
    bool Started_ = false;
    bool Stopped_ = false;
    TPromise<void> SyncPromise_ = NewPromise<void>();

    THashMap<TCellTag, TCellId> ObservedCells_;

    void DoStart()
    {
        if (Started_) {
            return;
        }
        Started_ = true;
        SyncExecutor_->Start();
        SyncExecutor_->ScheduleOutOfBand();
    }

    void DoStop()
    {
        if (Stopped_) {
            return;
        }
        Stopped_ = true;
        SyncExecutor_->Stop();
    }

    void AddCell(TCellTag cellTag, TCellId cellId = {})
    {
        if (auto it = ObservedCells_.find(cellTag)) {
            if (it->second && cellId) {
                ValidateChaosCellIdDuplication(cellTag, it->second, cellId);
            } else if (!it->second) {
                it->second = cellId;
            }
        } else {
            InsertOrCrash(ObservedCells_, std::make_pair(cellTag, cellId));
        }
    }

    void DoSync()
    {
        try {
            YT_LOG_DEBUG("Started synchronizing chaos cells in cell directory");

            auto connection = Connection_.Lock();
            if (!connection) {
                THROW_ERROR_EXCEPTION("Unable to synchronize chaos cells in cell directory: connection terminated");
            }

            auto masterChannel = connection->GetMasterChannelOrThrow(NApi::EMasterChannelKind::Follower, PrimaryMasterCellTagSentinel);
            auto proxy = TChaosMasterServiceProxy(std::move(masterChannel));
            auto req = proxy.GetCellDescriptors();

            auto rsp = WaitFor(req->Invoke())
                .ValueOrThrow();

            auto cellDescriptors = FromProto<std::vector<TCellDescriptor>>(rsp->cell_descriptors());

            THashMap<TCellTag, TCellId> observedCells;
            {
                auto guard = Guard(SpinLock_);
                observedCells = ObservedCells_;
            }

            for (auto descriptor : cellDescriptors) {
                auto cellTag = CellTagFromId(descriptor.CellId);
                TCellId cellId;
                {
                    auto guard = Guard(SpinLock_);
                    if (auto it = ObservedCells_.find(cellTag)) {
                        cellId = it->second;
                    }
                }

                if (cellId) {
                    ValidateChaosCellIdDuplication(cellTag, descriptor.CellId, cellId);
                } else {
                    auto guard = Guard(SpinLock_);
                    ObservedCells_[cellTag] = descriptor.CellId;
                }

                CellDirectory_->ReconfigureCell(descriptor);
            }

        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error synchronizing chaos cells in cell directory")
                << ex;
        }

        YT_LOG_DEBUG("Finished synchronizing chaos cells in cell directory");
    }

    void OnSync()
    {
        TError error;
        TPromise<void> syncPromise;

        {
            auto guard = Guard(SpinLock_);
            syncPromise = std::exchange(SyncPromise_, NewPromise<void>());
        }

        try {
            DoSync();
        } catch (const std::exception& ex) {
            error = TError(ex);
            YT_LOG_DEBUG(error);
        }

        syncPromise.Set(error);
    }

    void ValidateChaosCellIdDuplication(
        TCellTag cellTag,
        TCellId existingCellId,
        TCellId newCellId)
    {
        if (newCellId != existingCellId) {
            YT_LOG_ALERT("Duplicate chaos cell id (CellTag: %v, ExistingCellId: %v, NewCellId: %v)",
                cellTag,
                existingCellId,
                newCellId);
            THROW_ERROR_EXCEPTION("Duplicate chaos cell id for tag %v", cellTag)
                << TErrorAttribute("existing_cell_id", existingCellId)
                << TErrorAttribute("new_cell_id", newCellId);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IChaosCellDirectorySynchronizerPtr CreateChaosCellDirectorySynchronizer(
    TChaosCellDirectorySynchronizerConfigPtr config,
    ICellDirectoryPtr cellDirectory,
    IConnectionPtr connection,
    NLogging::TLogger logger)
{
    return New<TChaosCellDirectorySynchronizer>(
        std::move(config),
        std::move(cellDirectory),
        std::move(connection),
        std::move(logger));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosClient
