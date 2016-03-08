#include "tablet_manager.h"
#include "private.h"
#include "automaton.h"
#include "sorted_chunk_store.h"
#include "config.h"
#include "sorted_dynamic_store.h"
#include "in_memory_manager.h"
#include "lookup.h"
#include "partition.h"
#include "security_manager.h"
#include "slot_manager.h"
#include "store_flusher.h"
#include "sorted_store_manager.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "transaction.h"
#include "transaction_manager.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/data_node/chunk_block_manager.h>

#include <yt/server/hive/hive_manager.h>
#include <yt/server/hive/transaction_supervisor.pb.h>

#include <yt/server/hydra/hydra_manager.h>
#include <yt/server/hydra/mutation.h>
#include <yt/server/hydra/mutation_context.h>

#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/server/tablet_node/tablet_manager.pb.h>
#include <yt/server/tablet_node/transaction_manager.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/name_table.h>

#include <yt/ytlib/tablet_client/config.h>
#include <yt/ytlib/tablet_client/wire_protocol.h>
#include <yt/ytlib/tablet_client/wire_protocol.pb.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/timestamp_provider.h>

#include <yt/ytlib/api/client.h>

#include <yt/core/compression/codec.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/ring_queue.h>
#include <yt/core/misc/string.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletNode {

using namespace NCompression;
using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NHydra;
using namespace NCellNode;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NTabletNode::NProto;
using namespace NTabletServer::NProto;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NHive;
using namespace NHive::NProto;
using namespace NQueryClient;

////////////////////////////////////////////////////////////////////////////////

static const auto BlockedRowWaitQuantum = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TTabletAutomatonPart
{
public:
    explicit TImpl(
        TTabletManagerConfigPtr config,
        TTabletSlotPtr slot,
        TBootstrap* bootstrap)
        : TTabletAutomatonPart(
            slot,
            bootstrap)
        , Config_(config)
        , ChangelogCodec_(GetCodec(Config_->ChangelogCodec))
        , TabletContext_(this)
        , TabletMap_(TTabletMapTraits(this))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Slot_->GetAutomatonInvoker(), AutomatonThread);

        RegisterLoader(
            "TabletManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TabletManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));
        RegisterLoader(
            "TabletManager.Async",
            BIND(&TImpl::LoadAsync, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TabletManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TabletManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
        RegisterSaver(
            EAsyncSerializationPriority::Default,
            "TabletManager.Async",
            BIND(&TImpl::SaveAsync, Unretained(this)));

        RegisterMethod(BIND(&TImpl::HydraMountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUnmountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRemountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetTabletState, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraFollowerExecuteWrite, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRotateStore, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCommitTabletStoresUpdate, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletStoresUpdated, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSplitPartition, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraMergePartitions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdatePartitionSampleKeys, Unretained(this)));
    }

    void Initialize()
    {
        auto transactionManager = Slot_->GetTransactionManager();
        transactionManager->SubscribeTransactionPrepared(BIND(&TImpl::OnTransactionPrepared, MakeStrong(this)));
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionCommitted, MakeStrong(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionAborted, MakeStrong(this)));
    }


    TTablet* GetTabletOrThrow(const TTabletId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* tablet = FindTablet(id);
        if (!tablet) {
            THROW_ERROR_EXCEPTION("No such tablet %v", id);
        }
        return tablet;
    }


    void Read(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor,
        TWireProtocolReader* reader,
        TWireProtocolWriter* writer)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ValidateReadTimestamp(timestamp);

        while (!reader->IsFinished()) {
            ExecuteSingleRead(
                tabletSnapshot,
                timestamp,
                workloadDescriptor,
                reader,
                writer);
        }
    }

    void Write(
        TTabletSnapshotPtr tabletSnapshot,
        const TTransactionId& transactionId,
        TWireProtocolReader* reader,
        TFuture<void>* commitResult)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // NB: No yielding beyond this point.
        // May access tablet and transaction.

        auto* tablet = GetTabletOrThrow(tabletSnapshot->TabletId);

        tablet->ValidateMountRevision(tabletSnapshot->MountRevision);
        ValidateTabletMounted(tablet);
        ValidateTabletStoreLimit(tablet);
        ValidateMemoryLimit();

        auto atomicity = AtomicityFromTransactionId(transactionId);
        switch (atomicity) {
            case EAtomicity::Full:
                WriteAtomic(tablet, transactionId, reader, commitResult);
                break;

            case EAtomicity::None:
                ValidateClientTimestamp(transactionId);
                WriteNonAtomic(tablet, transactionId, reader, commitResult);
                break;

            default:
                YUNREACHABLE();
        }
    }


    void ScheduleStoreRotation(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& storeManager = tablet->GetStoreManager();
        if (!storeManager->IsRotationPossible()) {
            return;
        }

        storeManager->ScheduleRotation();

        TReqRotateStore request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        CommitTabletMutation(request);
    }


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        BuildYsonFluently(consumer)
            .DoMapFor(TabletMap_, [&] (TFluentMap fluent, const std::pair<TTabletId, TTablet*>& pair) {
                auto* tablet = pair.second;
                fluent
                    .Item(ToString(tablet->GetId()))
                    .Do(BIND(&TImpl::BuildTabletOrchidYson, Unretained(this), tablet));
            });
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet, TTabletId);

private:
    const TTabletManagerConfigPtr Config_;

    ICodec* const ChangelogCodec_;

    class TTabletContext
        : public ITabletContext
    {
    public:
        explicit TTabletContext(TImpl* owner)
            : Owner_(owner)
        { }

        virtual TCellId GetCellId() override
        {
            return Owner_->Slot_->GetCellId();
        }

        virtual TColumnEvaluatorCachePtr GetColumnEvaluatorCache() override
        {
            return Owner_->Bootstrap_->GetColumnEvaluatorCache();
        }

        virtual TObjectId GenerateId(EObjectType type) override
        {
            return Owner_->Slot_->GenerateId(type);
        }

        virtual IStorePtr CreateStore(TTablet* tablet, EStoreType type, const TStoreId& storeId) override
        {
            return Owner_->CreateStore(tablet, type, storeId, nullptr);
        }

        virtual IStoreManagerPtr CreateStoreManager(TTablet* tablet) override
        {
            return Owner_->CreateStoreManager(tablet);
        }

    private:
        TImpl* const Owner_;

    };

    class TTabletMapTraits
    {
    public:
        explicit TTabletMapTraits(TImpl* owner)
            : Owner_(owner)
        { }

        std::unique_ptr<TTablet> Create(const TTabletId& id) const
        {
            return std::make_unique<TTablet>(id, &Owner_->TabletContext_);
        }

    private:
        TImpl* const Owner_;

    };

    TTimestamp LastCommittedTimestamp_ = MinTimestamp;
    TTabletContext TabletContext_;
    TEntityMap<TTabletId, TTablet, TTabletMapTraits> TabletMap_;
    yhash_set<TTablet*> UnmountingTablets_;

    yhash_set<TSortedDynamicStorePtr> OrphanedStores_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(TSaveContext& context) const
    {
        TabletMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        using NYT::Save;

        Save(context, LastCommittedTimestamp_);
        TabletMap_.SaveValues(context);
    }

    TCallback<void(TSaveContext&)> SaveAsync()
    {
        std::vector<std::pair<TTabletId, TCallback<void(TSaveContext&)>>> capturedTablets;
        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            capturedTablets.push_back(std::make_pair(tablet->GetId(), tablet->AsyncSave()));
        }

        return BIND(
            [
                capturedTablets = std::move(capturedTablets)
            ] (TSaveContext& context) {
                using NYT::Save;
                for (const auto& pair : capturedTablets) {
                    Save(context, pair.first);
                    pair.second.Run(context);
                }
            });
    }

    void LoadKeys(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        Load(context, LastCommittedTimestamp_);
        TabletMap_.LoadValues(context);
    }

    void LoadAsync(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        SERIALIZATION_DUMP_WRITE(context, "tablets[%v]", TabletMap_.size());
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != TabletMap_.size(); ++index) {
                auto tabletId = LoadSuspended<TTabletId>(context);
                auto* tablet = GetTablet(tabletId);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", tabletId);
                SERIALIZATION_DUMP_INDENT(context) {
                    tablet->AsyncLoad(context);
                }
            }
        }
    }


    virtual void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnAfterSnapshotLoaded();

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            if (tablet->GetState() >= ETabletState::WaitingForLocks) {
                YCHECK(UnmountingTablets_.insert(tablet).second);
            }
        }

        auto transactionManager = Slot_->GetTransactionManager();
        for (const auto& pair : transactionManager->Transactions()) {
            auto* transaction = pair.second;
            int rowCount = 0;
            for (const auto& record : transaction->WriteLog()) {
                auto* tablet = FindTablet(record.TabletId);
                // NB: Tablet could be missing if it was e.g. forcefully removed.
                if (!tablet)
                    continue;

                TWireProtocolReader reader(record.Data);
                const auto& storeManager = tablet->GetStoreManager();
                while (!reader.IsFinished()) {
                    storeManager->ExecuteAtomicWrite(tablet, transaction, &reader, false);
                    ++rowCount;
                }
            }
            LOG_DEBUG_IF(rowCount > 0, "Transaction write log applied (TransactionId: %v, RowCount: %v)",
                transaction->GetId(),
                rowCount);

            if (transaction->GetState() == ETransactionState::PersistentCommitPrepared) {
                OnTransactionPrepared(transaction);
            }
        }
    }

    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::Clear();

        TabletMap_.Clear();
        UnmountingTablets_.clear();
        OrphanedStores_.clear();
    }


    virtual void OnLeaderRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderRecoveryComplete();

        StartEpoch();
    }

    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderActive();

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            CheckIfFullyUnlocked(tablet);
            CheckIfFullyFlushed(tablet);
        }
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopLeading();

        auto transactionManager = Slot_->GetTransactionManager();
        for (const auto& pair : transactionManager->Transactions()) {
            auto* transaction = pair.second;
            while (!transaction->PrelockedRows().empty()) {
                auto rowRef = transaction->PrelockedRows().front();
                transaction->PrelockedRows().pop();
                if (ValidateAndDiscardRowRef(rowRef)) {
                    rowRef.StoreManager->AbortRow(transaction, rowRef);
                }
            }
        }

        StopEpoch();
    }


    virtual void OnFollowerRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnFollowerRecoveryComplete();

        StartEpoch();
    }

    virtual void OnStopFollowing() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopFollowing();

        StopEpoch();
    }


    void StartEpoch()
    {
        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            StartTabletEpoch(tablet);
        }
    }

    void StopEpoch()
    {
        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            StopTabletEpoch(tablet);
        }
    }


    void HydraMountTablet(const TReqMountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto mountRevision = request.mount_revision();
        auto tableId = FromProto<TObjectId>(request.table_id());
        auto schema = FromProto<TTableSchema>(request.schema());
        auto keyColumns = FromProto<TKeyColumns>(request.key_columns());
        auto pivotKey = FromProto<TOwningKey>(request.pivot_key());
        auto nextPivotKey = FromProto<TOwningKey>(request.next_pivot_key());
        auto mountConfig = DeserializeTableMountConfig((TYsonString(request.mount_config())), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request.writer_options()), tabletId);
        auto atomicity = EAtomicity(request.atomicity());

        auto tabletHolder = std::make_unique<TTablet>(
            mountConfig,
            writerOptions,
            tabletId,
            mountRevision,
            tableId,
            &TabletContext_,
            schema,
            keyColumns,
            pivotKey,
            nextPivotKey,
            atomicity);

        auto* tablet = TabletMap_.Insert(tabletId, std::move(tabletHolder));

        tablet->CreateInitialPartition();
        tablet->SetState(ETabletState::Mounted);

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->CreateActiveStore();

        std::vector<std::tuple<TOwningKey, int, int>> chunkBoundaries;

        int descriptorIndex = 0;
        for (const auto& descriptor : request.stores()) {
            const auto& extensions = descriptor.chunk_meta().extensions();
            auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(extensions);
            if (miscExt.has_max_timestamp()) {
                UpdateLastCommittedTimestamp(miscExt.max_timestamp());
            }
            if (!miscExt.eden()) {
                auto boundaryKeysExt = GetProtoExtension<NTableClient::NProto::TBoundaryKeysExt>(extensions);
                auto minKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.min()), keyColumns.size());
                auto maxKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.max()), keyColumns.size());
                chunkBoundaries.push_back(std::make_tuple(minKey, -1, descriptorIndex));
                chunkBoundaries.push_back(std::make_tuple(maxKey, 1, descriptorIndex));
            }
            ++descriptorIndex;
        }

        if (!chunkBoundaries.empty()) {
            std::sort(chunkBoundaries.begin(), chunkBoundaries.end());
            std::vector<TOwningKey> pivotKeys{pivotKey};
            int depth = 0;
            for (const auto& boundary : chunkBoundaries) {
                if (std::get<1>(boundary) == -1 && depth == 0 && std::get<0>(boundary) > pivotKey) {
                    pivotKeys.push_back(std::get<0>(boundary));
                }
                depth -= std::get<1>(boundary);
            }
            YCHECK(tablet->Partitions().size() == 1);
            SplitTabletPartition(tablet, 0, pivotKeys);
        }

        for (const auto& descriptor : request.stores()) {
            auto type = EStoreType(descriptor.store_type());
            auto storeId = FromProto < TChunkId > (descriptor.store_id());
            YCHECK(descriptor.has_chunk_meta());
            YCHECK(!descriptor.has_backing_store_id());
            auto store = CreateStore(
                tablet,
                type,
                storeId,
                &descriptor.chunk_meta());
            storeManager->AddStore(store->AsChunk(), true);
        }

        SchedulePartitionsSampling(tablet);

        {
            TRspMountTablet response;
            ToProto(response.mutable_tablet_id(), tabletId);
            PostMasterMutation(response);
        }

        if (!IsRecovery()) {
            StartTabletEpoch(tablet);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet mounted (TabletId: %v, MountRevision: %x, TableId: %v, Keys: %v .. %v,"
            "StoreCount: %v, PartitionCount: %v, Atomicity: %v)",
            tabletId,
            mountRevision,
            tableId,
            pivotKey,
            nextPivotKey,
            request.stores_size(),
            tablet->Partitions().size(),
            tablet->GetAtomicity());
    }

    void HydraUnmountTablet(const TReqUnmountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        if (request.force()) {
            LOG_INFO_UNLESS(IsRecovery(), "Tablet is forcefully unmounted (TabletId: %v)",
                tabletId);

            // Just a formality.
            tablet->SetState(ETabletState::Unmounted);

            for (const auto& pair : tablet->Stores()) {
                SetStoreOrphaned(tablet, pair.second);
            }

            const auto& storeManager = tablet->GetStoreManager();
            for (const auto& store : storeManager->GetLockedStores()) {
                SetStoreOrphaned(tablet, store);
            }

            if (!IsRecovery()) {
                StopTabletEpoch(tablet);
            }

            TabletMap_.Remove(tabletId);
            UnmountingTablets_.erase(tablet); // don't check the result
            return;
        }

        if (tablet->GetState() != ETabletState::Mounted) {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to unmount a tablet in %Qlv state, ignored (TabletId: %v)",
                tablet->GetState(),
                tabletId);
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "Unmounting tablet (TabletId: %v)",
            tabletId);

        // Just a formality.
        YCHECK(tablet->GetState() == ETabletState::Mounted);
        tablet->SetState(ETabletState::WaitingForLocks);

        YCHECK(UnmountingTablets_.insert(tablet).second);

        LOG_INFO_IF(IsLeader(), "Waiting for all tablet locks to be released (TabletId: %v)",
            tabletId);

        if (IsLeader()) {
            CheckIfFullyUnlocked(tablet);
        }
    }

    void HydraRemountTablet(const TReqRemountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountConfig = DeserializeTableMountConfig((TYsonString(request.mount_config())), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request.writer_options()), tabletId);

        if (mountConfig->ReadOnly && !tablet->GetConfig()->ReadOnly) {
            RotateStores(tablet, true);
        }

        int oldSamplesPerPartition = tablet->GetConfig()->SamplesPerPartition;
        int newSamplesPerPartition = mountConfig->SamplesPerPartition;

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->Remount(mountConfig, writerOptions);

        if (oldSamplesPerPartition != newSamplesPerPartition) {
            SchedulePartitionsSampling(tablet);
        }

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet remounted (TabletId: %v)",
            tabletId);
    }

    void HydraSetTabletState(const TReqSetTabletState& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto requestedState = ETabletState(request.state());

        switch (requestedState) {
            case ETabletState::Flushing: {
                tablet->SetState(ETabletState::Flushing);

                // NB: Flush requests for all other stores must already be on their way.
                RotateStores(tablet, false);

                LOG_INFO_IF(IsLeader(), "Waiting for all tablet stores to be flushed (TabletId: %v)",
                    tabletId);

                if (IsLeader()) {
                    CheckIfFullyFlushed(tablet);
                }
                break;
            }

            case ETabletState::Unmounted: {
                tablet->SetState(ETabletState::Unmounted);

                LOG_INFO_UNLESS(IsRecovery(), "Tablet unmounted (TabletId: %v)",
                    tabletId);

                if (!IsRecovery()) {
                    StopTabletEpoch(tablet);
                }

                TabletMap_.Remove(tabletId);
                YCHECK(UnmountingTablets_.erase(tablet) == 1);

                {
                    TRspUnmountTablet response;
                    ToProto(response.mutable_tablet_id(), tabletId);
                    PostMasterMutation(response);
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    void HydraLeaderExecuteWriteAtomic(
        const TTransactionId& transactionId,
        int rowCount,
        const TTransactionWriteRecord& writeRecord)
    {
        auto transactionManager = Slot_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransaction(transactionId);

        for (int index = 0; index < rowCount; ++index) {
            YASSERT(!transaction->PrelockedRows().empty());
            auto rowRef = transaction->PrelockedRows().front();
            transaction->PrelockedRows().pop();

            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.StoreManager->ConfirmRow(transaction, rowRef);
            }
        }

        transaction->WriteLog().Enqueue(writeRecord);

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows confirmed (TabletId: %v, TransactionId: %v, RowCount: %v, WriteRecordSize: %v)",
            writeRecord.TabletId,
            transactionId,
            rowCount,
            writeRecord.Data.Size());
    }

    void HydraLeaderExecuteWriteNonAtomic(
        const TTabletId& tabletId,
        i64 mountRevision,
        const TTransactionId& transactionId,
        const TSharedRef& recordData)
    {
        auto* tablet = FindTablet(tabletId);
        // NB: Tablet could be missing if it was e.g. forcefully removed.
        if (!tablet) {
            return;
        }

        tablet->ValidateMountRevision(mountRevision);

        auto commitTimestamp = TimestampFromTransactionId(transactionId);
        auto adjustedCommitTimestamp = AdjustCommitTimestamp(commitTimestamp);

        TWireProtocolReader reader(recordData);
        int rowCount = 0;
        const auto& storeManager = tablet->GetStoreManager();
        while (!reader.IsFinished()) {
            storeManager->ExecuteNonAtomicWrite(tablet, adjustedCommitTimestamp, &reader);
            ++rowCount;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows written (TransactionId: %v, TabletId: %v, RowCount: %v, "
            "WriteRecordSize: %v)",
            transactionId,
            tabletId,
            rowCount,
            recordData.Size());
    }

    void HydraFollowerExecuteWrite(const TReqExecuteWrite& request) noexcept
    {
        auto transactionId = FromProto<TTransactionId>(request.transaction_id());
        auto atomicity = AtomicityFromTransactionId(transactionId);

        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        // NB: Tablet could be missing if it was e.g. forcefully removed.
        if (!tablet) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto codecId = ECodec(request.codec());
        auto* codec = GetCodec(codecId);
        auto compressedRecordData = TSharedRef::FromString(request.compressed_data());
        auto recordData = codec->Decompress(compressedRecordData);

        TWireProtocolReader reader(recordData);
        int rowCount = 0;

        const auto& storeManager = tablet->GetStoreManager();

        switch (atomicity) {
            case EAtomicity::Full: {
                auto transactionManager = Slot_->GetTransactionManager();
                auto* transaction = transactionManager->GetTransaction(transactionId);

                auto writeRecord = TTransactionWriteRecord{tabletId, recordData};

                while (!reader.IsFinished()) {
                    storeManager->ExecuteAtomicWrite(tablet, transaction, &reader, false);
                    ++rowCount;
                }

                transaction->WriteLog().Enqueue(writeRecord);
            }

            case EAtomicity::None: {
                auto commitTimestamp = TimestampFromTransactionId(transactionId);
                auto adjustedCommitTimestamp = AdjustCommitTimestamp(commitTimestamp);
                while (!reader.IsFinished()) {
                    storeManager->ExecuteNonAtomicWrite(tablet, adjustedCommitTimestamp, &reader);
                    ++rowCount;
                }
                break;
            }

            default:
                YUNREACHABLE();
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows written (TransactionId: %v, TabletId: %v, RowCount: %v, WriteRecordSize: %v)",
            transactionId,
            tabletId,
            rowCount,
            recordData.Size());
    }

    void HydraRotateStore(const TReqRotateStore& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }
        if (tablet->GetState() != ETabletState::Mounted) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        RotateStores(tablet, true);
        UpdateTabletSnapshot(tablet);
    }


    void HydraCommitTabletStoresUpdate(const TReqCommitTabletStoresUpdate& commitRequest)
    {
        auto tabletId = FromProto<TTabletId>(commitRequest.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = commitRequest.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        SmallVector<TStoreId, TypicalStoreIdCount> storeIdsToAdd;
        for (const auto& descriptor : commitRequest.stores_to_add()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToAdd.push_back(storeId);
        }

        SmallVector<TStoreId, TypicalStoreIdCount> storeIdsToRemove;
        for (const auto& descriptor : commitRequest.stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToRemove.push_back(storeId);
            auto store = tablet->GetStore(storeId);
            YCHECK(store->GetStoreState() != EStoreState::ActiveDynamic);
            store->SetStoreState(EStoreState::RemoveCommitting);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Committing tablet stores update "
            "(TabletId: %v, StoreIdsToAdd: %v, StoreIdsToRemove: %v)",
            tabletId,
            storeIdsToAdd,
            storeIdsToRemove);

        auto hiveManager = Slot_->GetHiveManager();
        auto* masterMailbox = Slot_->GetMasterMailbox();

        {
            TReqUpdateTabletStores masterRequest;
            ToProto(masterRequest.mutable_tablet_id(), tabletId);
            masterRequest.set_mount_revision(mountRevision);
            masterRequest.mutable_stores_to_add()->MergeFrom(commitRequest.stores_to_add());
            masterRequest.mutable_stores_to_remove()->MergeFrom(commitRequest.stores_to_remove());

            hiveManager->PostMessage(masterMailbox, masterRequest);
        }

        if (commitRequest.has_transaction_id()) {
            auto transactionId = FromProto<TTransactionId>(commitRequest.transaction_id());

            TReqHydraAbortTransaction masterRequest;
            ToProto(masterRequest.mutable_transaction_id(), transactionId);
            ToProto(masterRequest.mutable_mutation_id(), NRpc::NullMutationId);

            hiveManager->PostMessage(masterMailbox, masterRequest);
        }
    }

    void HydraOnTabletStoresUpdated(const TRspUpdateTabletStores& response)
    {
        auto tabletId = FromProto<TTabletId>(response.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = response.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();

        if (response.has_error()) {
            auto error = FromProto<TError>(response.error());
            LOG_WARNING_UNLESS(IsRecovery(), error, "Error updating tablet stores (TabletId: %v)",
                tabletId);

            for (const auto& descriptor : response.stores_to_remove()) {
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                auto store = tablet->GetStore(storeId);

                YCHECK(store->GetStoreState() == EStoreState::RemoveCommitting);
                switch (store->GetType()) {
                    case EStoreType::SortedDynamic: {
                        store->SetStoreState(EStoreState::PassiveDynamic);
                        break;
                    }
                    case EStoreType::SortedChunk: {
                        store->SetStoreState(EStoreState::Persistent);
                        break;
                    }
                }

                if (IsLeader()) {
                    storeManager->BackoffStoreRemoval(store);
                }
            }

            if (IsLeader()) {
                CheckIfFullyFlushed(tablet);
            }
            return;
        }

        auto mountConfig = tablet->GetConfig();
        auto inMemoryManager = Bootstrap_->GetInMemoryManager();
        std::vector<TStoreId> addedStoreIds;
        for (const auto& descriptor : response.stores_to_add()) {
            auto storeType = EStoreType(descriptor.store_type());
            auto storeId = FromProto<TChunkId>(descriptor.store_id());
            addedStoreIds.push_back(storeId);

            auto store = CreateStore(tablet, storeType, storeId, &descriptor.chunk_meta());
            storeManager->AddStore(store, false);

            // XXX(babenko): get rid of this
            auto chunkStore = store->AsSortedChunk();
            SchedulePartitionSampling(chunkStore->GetPartition());

            TStoreId backingStoreId;
            if (!IsRecovery() && descriptor.has_backing_store_id()) {
                backingStoreId = FromProto<TStoreId>(descriptor.backing_store_id());
                auto backingStore = tablet->GetStore(backingStoreId)->AsSorted();
                SetBackingStore(tablet, chunkStore, backingStore);
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Store added (TabletId: %v, StoreId: %v, BackingStoreId: %v)",
                tabletId,
                storeId,
                backingStoreId);
        }

        std::vector<TStoreId> removedStoreIds;
        for (const auto& descriptor : response.stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            removedStoreIds.push_back(storeId);

            auto store = tablet->GetStore(storeId);
            // XXX(babenko): consider moving to store manager
            if (store->IsSorted()) {
                auto sortedStore = store->AsSorted();
                SchedulePartitionSampling(sortedStore->GetPartition());
            }
            storeManager->RemoveStore(store);

            LOG_DEBUG_UNLESS(IsRecovery(), "Store removed (TabletId: %v, StoreId: %v)",
                tabletId,
                storeId);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet stores updated successfully "
            "(TabletId: %v, AddedStoreIds: %v, RemovedStoreIds: %v)",
            tabletId,
            addedStoreIds,
            removedStoreIds);

        UpdateTabletSnapshot(tablet);
        if (IsLeader()) {
            CheckIfFullyFlushed(tablet);
        }
    }

    void HydraSplitPartition(const TReqSplitPartition& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto partitionId = FromProto<TPartitionId>(request.partition_id());
        auto* partition = tablet->GetPartitionById(partitionId);
        auto pivotKeys = FromProto<std::vector<TOwningKey>>(request.pivot_keys());

        // NB: Set the state back to normal; otherwise if some of the below checks fail, we might get
        // a partition stuck in splitting state forever.
        partition->SetState(EPartitionState::Normal);

        if (tablet->Partitions().size() >= tablet->GetConfig()->MaxPartitionCount)
            return;

        int partitionIndex = partition->GetIndex();
        i64 partitionDataSize = partition->GetUncompressedDataSize();

        SplitTabletPartition(tablet, partitionIndex, pivotKeys);

        auto resultingPartitionIds = JoinToString(
            tablet->Partitions().begin() + partitionIndex,
            tablet->Partitions().begin() + partitionIndex + pivotKeys.size(),
            TPartitionIdFormatter());

        LOG_INFO_UNLESS(IsRecovery(), "Splitting partition (TabletId: %v, OriginalPartitionId: %v, "
            "ResultingPartitionIds: %v, DataSize: %v, Keys: %v)",
            tablet->GetId(),
            partitionId,
            resultingPartitionIds,
            partitionDataSize,
            JoinToString(pivotKeys, STRINGBUF(" .. ")));

        // NB: Initial partition is split into new ones with indexes |[partitionIndex, partitionIndex + pivotKeys.size())|.
        SchedulePartitionsSampling(tablet, partitionIndex, partitionIndex + pivotKeys.size());
        UpdateTabletSnapshot(tablet);
    }

    void HydraMergePartitions(const TReqMergePartitions& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto firstPartitionId = FromProto<TPartitionId>(request.partition_id());
        auto* firstPartition = tablet->GetPartitionById(firstPartitionId);

        int firstPartitionIndex = firstPartition->GetIndex();
        int lastPartitionIndex = firstPartitionIndex + request.partition_count() - 1;

        i64 partitionsDataSize = 0;
        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            const auto& partition = tablet->Partitions()[index];
            partitionsDataSize += partition->GetUncompressedDataSize();
            // See HydraSplitPartition.
            // Currently this code is redundant since there's no escape path below,
            // but we prefer to keep it to make things look symmetric.
            partition->SetState(EPartitionState::Normal);
        }

        auto originalPartitionIds = JoinToString(
            tablet->Partitions().begin() + firstPartitionIndex,
            tablet->Partitions().begin() + lastPartitionIndex + 1,
            TPartitionIdFormatter());

        MergeTabletPartitions(tablet, firstPartitionIndex, lastPartitionIndex);

        LOG_INFO_UNLESS(IsRecovery(), "Merging partitions (TabletId: %v, OriginalPartitionIds: %v, "
            "ResultingPartitionId: %v, DataSize: %v)",
            tablet->GetId(),
            originalPartitionIds,
            tablet->Partitions()[firstPartitionIndex]->GetId(),
            partitionsDataSize);

        // NB: Initial partitions are merged into a single one with index |firstPartitionIndex|.
        SchedulePartitionsSampling(tablet, firstPartitionIndex, firstPartitionIndex + 1);
        UpdateTabletSnapshot(tablet);
    }

    void HydraUpdatePartitionSampleKeys(const TReqUpdatePartitionSampleKeys& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request.mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto partitionId = FromProto<TPartitionId>(request.partition_id());
        auto* partition = tablet->FindPartitionById(partitionId);
        if (!partition) {
            return;
        }

        auto sampleKeys = New<TKeyList>();
        sampleKeys->Keys = FromProto<std::vector<TOwningKey>>(request.sample_keys());
        partition->SetSampleKeys(sampleKeys);
        YCHECK(sampleKeys->Keys.empty() || sampleKeys->Keys[0] > partition->GetPivotKey());
        UpdateTabletSnapshot(tablet);

        const auto* mutationContext = GetCurrentMutationContext();
        partition->SetSamplingTime(mutationContext->GetTimestamp());

        LOG_INFO_UNLESS(IsRecovery(), "Partition sample keys updated (TabletId: %v, PartitionId: %v, SampleKeyCount: %v)",
            tabletId,
            partition->GetId(),
            sampleKeys->Keys.size());
    }


    void OnTransactionPrepared(TTransaction* transaction)
    {
        auto handleRow = [&] (const TSortedDynamicRowRef& rowRef) {
            // NB: Don't call ValidateAndDiscardRowRef, row refs are just scanned.
            if (ValidateRowRef(rowRef)) {
                rowRef.StoreManager->PrepareRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        for (auto it = transaction->PrelockedRows().begin();
            it != transaction->PrelockedRows().end();
            transaction->PrelockedRows().move_forward(it))
        {
            handleRow(*it);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows prepared (TransactionId: %v, LockedRowCount: %v, PrelockedRowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size(),
            transaction->PrelockedRows().size());
    }

    void OnTransactionCommitted(TTransaction* transaction)
    {
        auto handleRow = [&] (const TSortedDynamicRowRef& rowRef) {
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.StoreManager->CommitRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows committed (TransactionId: %v, RowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size());

        YCHECK(transaction->PrelockedRows().empty());
        transaction->LockedRows().clear();

        UpdateLastCommittedTimestamp(transaction->GetCommitTimestamp());

        OnTransactionFinished(transaction);
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        auto handleRow = [&] (const TSortedDynamicRowRef& rowRef) {
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.StoreManager->AbortRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows aborted (TransactionId: %v, RowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size());

        YCHECK(transaction->PrelockedRows().empty());
        transaction->LockedRows().clear();

        OnTransactionFinished(transaction);
    }

    void OnTransactionFinished(TTransaction* /*transaction*/)
    {
        if (IsLeader()) {
            for (auto* tablet : UnmountingTablets_) {
                CheckIfFullyUnlocked(tablet);
            }
        }
    }


    void SetStoreOrphaned(TTablet* tablet, IStorePtr store)
    {
        if (store->GetStoreState() == EStoreState::Orphaned) {
            return;
        }

        store->SetStoreState(EStoreState::Orphaned);

        if (store->GetType() != EStoreType::SortedDynamic) {
            return;
        }
        
        auto dynamicStore = store->AsSortedDynamic();
        int lockCount = dynamicStore->GetLockCount();
        if (lockCount > 0) {
            YCHECK(OrphanedStores_.insert(dynamicStore).second);
            LOG_INFO_UNLESS(IsRecovery(), "Dynamic memory store is orphaned and will be kept (StoreId: %v, TabletId: %v, LockCount: %v)",
                store->GetId(),
                tablet->GetId(),
                lockCount);
        }
    }

    bool ValidateRowRef(const TSortedDynamicRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        return store->GetStoreState() != EStoreState::Orphaned;
    }

    bool ValidateAndDiscardRowRef(const TSortedDynamicRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        if (store->GetStoreState() != EStoreState::Orphaned) {
            return true;
        }

        int lockCount = store->Unlock();
        if (lockCount == 0) {
            LOG_INFO_UNLESS(IsRecovery(), "Store unlocked and will be dropped (StoreId: %v)",
                store->GetId());
            YCHECK(OrphanedStores_.erase(store) == 1);
        }

        return false;
    }


    void ExecuteSingleRead(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor,
        TWireProtocolReader* reader,
        TWireProtocolWriter* writer)
    {
        auto command = reader->ReadCommand();
        switch (command) {
            case EWireProtocolCommand::LookupRows:
                LookupRows(
                    std::move(tabletSnapshot),
                    timestamp,
                    workloadDescriptor,
                    reader,
                    writer);
                break;

            default:
                THROW_ERROR_EXCEPTION("Unknown read command %v",
                    command);
        }
    }


    void WriteAtomic(
        TTablet* tablet,
        const TTransactionId& transactionId,
        TWireProtocolReader* reader,
        TFuture<void>* commitResult)
    {
        const auto& tabletId = tablet->GetId();
        const auto& storeManager = tablet->GetStoreManager();

        auto transactionManager = Slot_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);
        ValidateTransactionActive(transaction);

        int prelockedCountBefore = transaction->PrelockedRows().size();
        auto readerBegin = reader->GetCurrent();

        TError error;
        TNullable<TRowBlockedException> rowBlockedEx;

        while (!reader->IsFinished()) {
            const char* readerCheckpoint = reader->GetCurrent();
            auto rewindReader = [&] () {
                reader->SetCurrent(readerCheckpoint);
            };
            try {
                storeManager->ExecuteAtomicWrite(tablet, transaction, reader, true);
            } catch (const TRowBlockedException& ex) {
                rewindReader();
                rowBlockedEx = ex;
                break;
            } catch (const std::exception& ex) {
                rewindReader();
                error = ex;
                break;
            }
        }

        int prelockedCountAfter = transaction->PrelockedRows().size();
        int prelockedCountDelta = prelockedCountAfter - prelockedCountBefore;
        if (prelockedCountDelta > 0) {
            LOG_DEBUG("Rows prelocked (TransactionId: %v, TabletId: %v, RowCount: %v)",
                transactionId,
                tabletId,
                prelockedCountDelta);

            auto readerEnd = reader->GetCurrent();
            auto recordData = reader->Slice(readerBegin, readerEnd);
            auto compressedRecordData = ChangelogCodec_->Compress(recordData);
            auto writeRecord = TTransactionWriteRecord{tabletId, recordData};

            TReqExecuteWrite hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            ToProto(hydraRequest.mutable_tablet_id(), tabletId);
            hydraRequest.set_mount_revision(tablet->GetMountRevision());
            hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
            hydraRequest.set_compressed_data(ToString(compressedRecordData));
            *commitResult = CreateMutation(Slot_->GetHydraManager(), hydraRequest)
                ->SetAction(
                    BIND(
                        &TImpl::HydraLeaderExecuteWriteAtomic,
                        MakeStrong(this),
                        transactionId,
                        prelockedCountDelta,
                        writeRecord))
                ->Commit()
                 .As<void>();
        }

        // NB: Yielding is now possible.
        // Cannot neither access tablet, nor transaction.

        if (rowBlockedEx) {
            rowBlockedEx->GetStore()->WaitOnBlockedRow(
                rowBlockedEx->GetRow(),
                rowBlockedEx->GetLockMask(),
                rowBlockedEx->GetTimestamp());
        }

        error.ThrowOnError();
    }

    void WriteNonAtomic(
        TTablet* tablet,
        const TTransactionId& transactionId,
        TWireProtocolReader* reader,
        TFuture<void>* commitResult)
    {
        // Get and skip the whole reader content.
        auto begin = reader->GetBegin();
        auto end = reader->GetEnd();
        auto recordData = reader->Slice(begin, end);
        reader->SetCurrent(end);

        auto compressedRecordData = ChangelogCodec_->Compress(recordData);

        TReqExecuteWrite hydraRequest;
        ToProto(hydraRequest.mutable_transaction_id(), transactionId);
        ToProto(hydraRequest.mutable_tablet_id(), tablet->GetId());
        hydraRequest.set_mount_revision(tablet->GetMountRevision());
        hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
        hydraRequest.set_compressed_data(ToString(compressedRecordData));
        *commitResult = CreateMutation(Slot_->GetHydraManager(), hydraRequest)
            ->SetAction(
                BIND(
                    &TImpl::HydraLeaderExecuteWriteNonAtomic,
                    MakeStrong(this),
                    tablet->GetId(),
                    tablet->GetMountRevision(),
                    transactionId,
                    recordData))
            ->Commit()
             .As<void>();
    }


    void CheckIfFullyUnlocked(TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::WaitingForLocks) {
            return;
        }

        if (tablet->GetStoreManager()->HasActiveLocks()) {
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "All tablet locks released (TabletId: %v)",
            tablet->GetId());

        tablet->SetState(ETabletState::FlushPending);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        request.set_state(static_cast<int>(ETabletState::Flushing));
        CommitTabletMutation(request);
    }

    void CheckIfFullyFlushed(TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Flushing) {
            return;
        }

        if (tablet->GetStoreManager()->HasUnflushedStores()) {
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "All tablet stores flushed (TabletId: %v)",
            tablet->GetId());

        tablet->SetState(ETabletState::UnmountPending);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        request.set_state(static_cast<int>(ETabletState::Unmounted));
        CommitTabletMutation(request);
    }


    void RotateStores(TTablet* tablet, bool createNew)
    {
        tablet->GetStoreManager()->Rotate(createNew);
    }


    void CommitTabletMutation(const ::google::protobuf::MessageLite& message)
    {
        auto mutation = CreateMutation(Slot_->GetHydraManager(), message);
        Slot_->GetEpochAutomatonInvoker()->Invoke(
            BIND(IgnoreResult(&TMutation::CommitAndLog), mutation, Logger));
    }

    void PostMasterMutation(const ::google::protobuf::MessageLite& message)
    {
        auto hiveManager = Slot_->GetHiveManager();
        hiveManager->PostMessage(Slot_->GetMasterMailbox(), message);
    }


    void StartTabletEpoch(TTablet* tablet)
    {
        const auto& storeManager = tablet->GetStoreManager();
        storeManager->StartEpoch(Slot_);

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->RegisterTabletSnapshot(Slot_, tablet);

        for (const auto& pair : tablet->Stores()) {
            const auto& store = pair.second;
            if (store->GetType() == EStoreType::SortedDynamic) {
                auto sortedDynamicStore = store->AsSortedDynamic();
                auto rowBlockedHandler = CreateRowBlockedHandler(
                    sortedDynamicStore,
                    tablet);
                sortedDynamicStore->SetRowBlockedHandler(rowBlockedHandler);
            }
        }
    }

    void StopTabletEpoch(TTablet* tablet)
    {
        const auto& storeManager = tablet->GetStoreManager();
        storeManager->StopEpoch();

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->UnregisterTabletSnapshot(Slot_, tablet);

        for (const auto& pair : tablet->Stores()) {
            const auto& store = pair.second;
            if (store->GetType() == EStoreType::SortedDynamic) {
                store->AsSortedDynamic()->ResetRowBlockedHandler();
            }
        }
    }


    void SplitTabletPartition(TTablet* tablet, int partitionIndex, const std::vector<TOwningKey>& pivotKeys)
    {
        tablet->SplitPartition(partitionIndex, pivotKeys);
        if (!IsRecovery()) {
            for (int currentIndex = partitionIndex; currentIndex < partitionIndex + pivotKeys.size(); ++currentIndex) {
                tablet->Partitions()[currentIndex]->StartEpoch();
            }
        }
    }

    void MergeTabletPartitions(TTablet* tablet, int firstIndex, int lastIndex)
    {
        tablet->MergePartitions(firstIndex, lastIndex);
        if (!IsRecovery()) {
            tablet->Partitions()[firstIndex]->StartEpoch();
        }
    }


    void SetBackingStore(TTablet* tablet, TSortedChunkStorePtr store, ISortedStorePtr backingStore)
    {
        store->SetBackingStore(backingStore);
        LOG_DEBUG("Backing store set (StoreId: %v, BackingStoreId: %v)",
            store->GetId(),
            backingStore->GetId());

        auto callback = BIND([=, this_ = MakeStrong(this)] () {
            VERIFY_THREAD_AFFINITY(AutomatonThread);
            store->SetBackingStore(nullptr);
            LOG_DEBUG("Backing store released (StoreId: %v)", store->GetId());
        });
        TDelayedExecutor::Submit(
            // NB: Submit the callback via the regular automaton invoker, not the epoch one since
            // we need the store to be released even if the epoch ends.
            callback.Via(Slot_->GetAutomatonInvoker()),
            tablet->GetConfig()->BackingStoreRetentionTime);
    }


    void BuildTabletOrchidYson(TTablet* tablet, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginAttributes()
                .Item("opaque").Value(true)
            .EndAttributes()
            .BeginMap()
                .Item("table_id").Value(tablet->GetTableId())
                .Item("state").Value(tablet->GetState())
                .Item("pivot_key").Value(tablet->GetPivotKey())
                .Item("next_pivot_key").Value(tablet->GetNextPivotKey())
                .Item("eden").Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), tablet->GetEden()))
                .Item("partitions").DoListFor(tablet->Partitions(), [&] (TFluentList fluent, const std::unique_ptr<TPartition>& partition) {
                    fluent
                        .Item()
                        .Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), partition.get()));
                })
            .EndMap();
    }

    void BuildPartitionOrchidYson(TPartition* partition, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("id").Value(partition->GetId())
                .Item("state").Value(partition->GetState())
                .Item("pivot_key").Value(partition->GetPivotKey())
                .Item("next_pivot_key").Value(partition->GetNextPivotKey())
                .Item("sample_key_count").Value(partition->GetSampleKeys()->Keys.size())
                .Item("sampling_time").Value(partition->GetSamplingTime())
                .Item("sampling_request_time").Value(partition->GetSamplingRequestTime())
                .Item("compaction_time").Value(partition->GetCompactionTime())
                .Item("uncompressed_data_size").Value(partition->GetUncompressedDataSize())
                .Item("unmerged_row_count").Value(partition->GetUnmergedRowCount())
                .Item("stores").DoMapFor(partition->Stores(), [&] (TFluentMap fluent, const IStorePtr& store) {
                    fluent
                        .Item(ToString(store->GetId()))
                        .Do(BIND(&TImpl::BuildStoreOrchidYson, Unretained(this), store));
                })
            .EndMap();
    }

    void BuildStoreOrchidYson(IStorePtr store, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginAttributes()
                .Item("opaque").Value(true)
            .EndAttributes()
            .BeginMap()
                .Do(BIND(&IStore::BuildOrchidYson, store))
            .EndMap();
    }


    static EMemoryCategory GetMemoryCategoryFromStore(IStorePtr store)
    {
        switch (store->GetType()) {
            case EStoreType::SortedDynamic:
                return EMemoryCategory::TabletDynamic;
            case EStoreType::SortedChunk:
                return EMemoryCategory::TabletStatic;
            default:
                YUNREACHABLE();
        }
    }

    static void OnStoreMemoryUsageUpdated(NCellNode::TBootstrap* bootstrap, EMemoryCategory category, i64 delta)
    {
        auto* tracker = bootstrap->GetMemoryUsageTracker();
        if (delta >= 0) {
            tracker->Acquire(category, delta);
        } else {
            tracker->Release(category, -delta);
        }
    }

    void StartMemoryUsageTracking(IStorePtr store)
    {
        store->SubscribeMemoryUsageUpdated(BIND(
            &TImpl::OnStoreMemoryUsageUpdated,
            Bootstrap_,
            GetMemoryCategoryFromStore(store)));
    }

    void ValidateMemoryLimit()
    {
        if (Bootstrap_->GetTabletSlotManager()->IsOutOfMemory()) {
            THROW_ERROR_EXCEPTION("Node is out of tablet memory, all writes disabled");
        }
    }

    void ValidateClientTimestamp(const TTransactionId& transactionId)
    {
        auto clientTimestamp = TimestampFromTransactionId(transactionId);
        auto timestampProvider = Bootstrap_->GetMasterClient()->GetConnection()->GetTimestampProvider();
        auto serverTimestamp = timestampProvider->GetLatestTimestamp();
        auto clientInstant = TimestampToInstant(clientTimestamp).first;
        auto serverInstant = TimestampToInstant(serverTimestamp).first;
        if (clientInstant > serverInstant + Config_->ClientTimestampThreshold ||
            clientInstant < serverInstant - Config_->ClientTimestampThreshold)
        {
            THROW_ERROR_EXCEPTION("Transaction timestamp is off limits, check the local clock readings")
                << TErrorAttribute("client_timestamp", clientTimestamp)
                << TErrorAttribute("server_timestamp", serverTimestamp);
        }
    }

    void ValidateTabletStoreLimit(TTablet* tablet)
    {
        auto storeCount = tablet->Stores().size();
        auto storeLimit = tablet->GetConfig()->MaxStoresPerTablet;
        if (storeCount >= storeLimit) {
            THROW_ERROR_EXCEPTION("Too many stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetTableId())
                << TErrorAttribute("store_count", storeCount)
                << TErrorAttribute("store_limit", storeLimit);
        }

        auto overlappingStoreCount = tablet->GetOverlappingStoreCount();
        auto overlappingStoreLimit = tablet->GetConfig()->MaxOverlappingStoreCount;
        if (overlappingStoreCount >= overlappingStoreLimit) {
            THROW_ERROR_EXCEPTION("Too many overlapping stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetTableId())
                << TErrorAttribute("overlapping_store_count", overlappingStoreCount)
                << TErrorAttribute("overlapping_store_limit", overlappingStoreLimit);
        }
    }


    void UpdateTabletSnapshot(TTablet* tablet)
    {
        if (!IsRecovery()) {
            auto slotManager = Bootstrap_->GetTabletSlotManager();
            slotManager->UpdateTabletSnapshot(Slot_, tablet);
        }
    }


    void SchedulePartitionSampling(TPartition* partition)
    {
        if (!partition->IsEden()) {
            const auto* mutationContext = GetCurrentMutationContext();
            partition->SetSamplingRequestTime(mutationContext->GetTimestamp());
        }
    }

    void SchedulePartitionsSampling(TTablet* tablet, int beginPartitionIndex, int endPartitionIndex)
    {
        const auto* mutationContext = GetCurrentMutationContext();
        for (int index = beginPartitionIndex; index < endPartitionIndex; ++index) {
            tablet->Partitions()[index]->SetSamplingRequestTime(mutationContext->GetTimestamp());
        }
    }

    void SchedulePartitionsSampling(TTablet* tablet)
    {
        SchedulePartitionsSampling(tablet, 0, tablet->Partitions().size());
    }


    void ValidateTabletMounted(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (tablet->GetState() != ETabletState::Mounted) {
            THROW_ERROR_EXCEPTION("Tablet %v is not in \"mounted\" state",
                tablet->GetId());
        }
    }

    void ValidateTransactionActive(TTransaction* transaction)
    {
        if (transaction->GetState() != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }
    }


    TTableMountConfigPtr DeserializeTableMountConfig(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTableMountConfigPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing tablet mount config (TabletId: %v)",
                 tabletId);
            return New<TTableMountConfig>();
        }
    }

    TTabletWriterOptionsPtr DeserializeTabletWriterOptions(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTabletWriterOptionsPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing writer options (TabletId: %v)",
                 tabletId);
            return New<TTabletWriterOptions>();
        }
    }


    void UpdateLastCommittedTimestamp(TTimestamp timestamp)
    {
        LastCommittedTimestamp_ = std::max(LastCommittedTimestamp_, timestamp);
    }

    TTimestamp AdjustCommitTimestamp(TTimestamp timestamp)
    {
        auto adjustedTimestamp = std::max(timestamp, LastCommittedTimestamp_ + 1);
        UpdateLastCommittedTimestamp(adjustedTimestamp);
        return adjustedTimestamp;
    }


    void OnRowBlocked(
        IStore* store,
        const TTabletId& tabletId,
        IInvokerPtr invoker,
        TSortedDynamicRow row,
        int lockIndex)
    {
        WaitFor(
            BIND(
                &TImpl::WaitOnBlockedRow,
                MakeStrong(this),
                MakeStrong(store),
                tabletId,
                row,
                lockIndex)
            .AsyncVia(invoker)
            .Run());
    }

    void WaitOnBlockedRow(
        IStorePtr /*store*/,
        const TTabletId& tabletId,
        TSortedDynamicRow row,
        int lockIndex)
    {
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        const auto& lock = row.BeginLocks(tablet->GetKeyColumnCount())[lockIndex];
        const auto* transaction = lock.Transaction;
        if (!transaction) {
            return;
        }

        LOG_DEBUG("Waiting on blocked row (Key: %v, LockIndex: %v, TabletId: %v, TransactionId: %v)",
            RowToKey(tablet->Schema(), tablet->KeyColumns(), row),
            lockIndex,
            tabletId,
            transaction->GetId());

        WaitFor(transaction->GetFinished().WithTimeout(BlockedRowWaitQuantum));
    }


    IStoreManagerPtr CreateStoreManager(TTablet* tablet)
    {
        // XXX(babenko): handle ordered tablets
        return New<TSortedStoreManager>(
            Config_,
            tablet,
            &TabletContext_,
            Slot_->GetHydraManager(),
            Bootstrap_->GetInMemoryManager());
    }

    IStorePtr CreateStore(
        TTablet* tablet,
        EStoreType type,
        const TStoreId& storeId,
        const TChunkMeta* chunkMeta)
    {
        auto store = DoCreateStore(tablet, type, storeId, chunkMeta);
        StartMemoryUsageTracking(store);
        return store;
    }

    IStorePtr DoCreateStore(
        TTablet* tablet,
        EStoreType type,
        const TStoreId& storeId,
        const TChunkMeta* chunkMeta)
    {
        switch (type) {
            case EStoreType::SortedChunk:
                return New<TSortedChunkStore>(
                    storeId,
                    tablet,
                    chunkMeta,
                    Bootstrap_);

            case EStoreType::SortedDynamic:
                return New<TSortedDynamicStore>(
                    Config_,
                    storeId,
                    tablet);

            default:
                YUNREACHABLE();
        }
    }

    TSortedDynamicStore::TRowBlockedHandler CreateRowBlockedHandler(
        const IStorePtr& store,
        TTablet* tablet)
    {
        return BIND(
            &TImpl::OnRowBlocked,
            MakeWeak(this),
            Unretained(store.Get()),
            tablet->GetId(),
            Slot_->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Read));
    }

};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTablet, TTabletId, TabletMap_)

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletManager(
    TTabletManagerConfigPtr config,
    TTabletSlotPtr slot,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        config,
        slot,
        bootstrap))
{ }

TTabletManager::~TTabletManager()
{ }

void TTabletManager::Initialize()
{
    Impl_->Initialize();
}

TTablet* TTabletManager::GetTabletOrThrow(const TTabletId& id)
{
    return Impl_->GetTabletOrThrow(id);
}

void TTabletManager::Read(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor,
    TWireProtocolReader* reader,
    TWireProtocolWriter* writer)
{
    Impl_->Read(
        std::move(tabletSnapshot),
        timestamp,
        workloadDescriptor,
        reader,
        writer);
}

void TTabletManager::Write(
    TTabletSnapshotPtr tabletSnapshot,
    const TTransactionId& transactionId,
    TWireProtocolReader* reader,
    TFuture<void>* commitResult)
{
    return Impl_->Write(
        std::move(tabletSnapshot),
        transactionId,
        reader,
        commitResult);
}

void TTabletManager::ScheduleStoreRotation(TTablet* tablet)
{
    Impl_->ScheduleStoreRotation(tablet);
}

void TTabletManager::BuildOrchidYson(IYsonConsumer* consumer)
{
    Impl_->BuildOrchidYson(consumer);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTablet, TTabletId, *Impl_)

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
