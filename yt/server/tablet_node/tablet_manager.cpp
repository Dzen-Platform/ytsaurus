#include "tablet_manager.h"
#include "private.h"
#include "automaton.h"
#include "sorted_chunk_store.h"
#include "ordered_chunk_store.h"
#include "config.h"
#include "sorted_dynamic_store.h"
#include "ordered_dynamic_store.h"
#include "replicated_store_manager.h"
#include "in_memory_manager.h"
#include "lookup.h"
#include "partition.h"
#include "security_manager.h"
#include "slot_manager.h"
#include "store_flusher.h"
#include "sorted_store_manager.h"
#include "ordered_store_manager.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "transaction.h"
#include "transaction_manager.h"
#include "table_replicator.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/master_connector.h>

#include <yt/server/hive/hive_manager.h>
#include <yt/server/hive/transaction_supervisor.pb.h>
#include <yt/server/hive/helpers.h>

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

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/compression/codec.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/ring_queue.h>
#include <yt/core/misc/string.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/virtual.h>

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
using namespace NHiveServer;
using namespace NHiveServer::NProto;
using namespace NQueryClient;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TTabletAutomatonPart
{
public:
    explicit TImpl(
        TTabletManagerConfigPtr config,
        TTabletSlotPtr slot,
        TBootstrap* bootstrap)
        : TCompositeAutomatonPart(
            slot->GetHydraManager(),
            slot->GetAutomaton(),
            slot->GetAutomatonInvoker())
        , TTabletAutomatonPart(
            slot,
            bootstrap)
        , Config_(config)
        , ChangelogCodec_(GetCodec(Config_->ChangelogCodec))
        , TabletContext_(this)
        , TabletMap_(TTabletMapTraits(this))
        , DynamicStoresMemoryTrackerGuard_(TNodeMemoryTrackerGuard::Acquire(
            Bootstrap_->GetMemoryUsageTracker(),
            EMemoryCategory::TabletDynamic,
            0,
            MemoryUsageGranularity))
        , StaticStoresMemoryTrackerGuard_(TNodeMemoryTrackerGuard::Acquire(
            Bootstrap_->GetMemoryUsageTracker(),
            EMemoryCategory::TabletStatic,
            0,
            MemoryUsageGranularity))
        , WriteLogsMemoryTrackerGuard_(TNodeMemoryTrackerGuard::Acquire(
            Bootstrap_->GetMemoryUsageTracker(),
            EMemoryCategory::TabletDynamic,
            0,
            MemoryUsageGranularity))
        , OrchidService_(TOrchidService::Create(MakeWeak(this), Slot_->GetGuardedAutomatonInvoker()))
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
        RegisterMethod(BIND(&TImpl::HydraFreezeTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUnfreezeTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetTabletState, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraFollowerWriteRows, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraTrimRows, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRotateStore, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSplitPartition, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraMergePartitions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdatePartitionSampleKeys, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraAddTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRemoveTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraEnableTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraDisableTableReplica, Unretained(this)));
    }

    void Initialize()
    {
        const auto& transactionManager = Slot_->GetTransactionManager();

        transactionManager->SubscribeTransactionPrepared(BIND(&TImpl::OnTransactionPrepared, MakeStrong(this)));
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionCommitted, MakeStrong(this)));
        transactionManager->SubscribeTransactionSerialized(BIND(&TImpl::OnTransactionSerialized, MakeStrong(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionAborted, MakeStrong(this)));
        transactionManager->SubscribeTransactionTransientReset(BIND(&TImpl::OnTransactionTransientReset, MakeStrong(this)));
        transactionManager->RegisterPrepareActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraPrepareReplicateRows, MakeStrong(this))));
        transactionManager->RegisterCommitActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraCommitReplicateRows, MakeStrong(this))));
        transactionManager->RegisterAbortActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraAbortReplicateRows, MakeStrong(this))));
        transactionManager->RegisterPrepareActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraPrepareUpdateTabletStores, MakeStrong(this))));
        transactionManager->RegisterCommitActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraCommitUpdateTabletStores, MakeStrong(this))));
        transactionManager->RegisterAbortActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraAbortUpdateTabletStores, MakeStrong(this))));

        // Initialize periodic latest timestamp update.
        const auto& timestampProvider = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetTimestampProvider();
        timestampProvider->GetLatestTimestamp();
    }


    TTablet* GetTabletOrThrow(const TTabletId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* tablet = FindTablet(id);
        if (!tablet) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::NoSuchTablet,
                "No such tablet %v",
                id);
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
        ValidateTabletRetainedTimestamp(tabletSnapshot, timestamp);

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
        TTimestamp transactionStartTimestamp,
        TDuration transactionTimeout,
        TTransactionSignature signature,
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
                WriteAtomic(
                    tablet,
                    transactionId,
                    transactionStartTimestamp,
                    transactionTimeout,
                    signature,
                    reader,
                    commitResult);
                break;

            case EAtomicity::None:
                ValidateClientTimestamp(transactionId);
                WriteNonAtomic(
                    tablet,
                    transactionId,
                    reader,
                    commitResult);
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    TFuture<void> Trim(
        TTabletSnapshotPtr tabletSnapshot,
        i64 trimmedRowCount)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        try {
            auto* tablet = GetTabletOrThrow(tabletSnapshot->TabletId);

            tablet->ValidateMountRevision(tabletSnapshot->MountRevision);
            ValidateTabletMounted(tablet);

            i64 totalRowCount = tablet->GetTotalRowCount();
            if (trimmedRowCount > totalRowCount) {
                THROW_ERROR_EXCEPTION("Cannot trim tablet %v at row %v since it only has %v row(s)",
                    tablet->GetId(),
                    trimmedRowCount,
                    totalRowCount);
            }

            NProto::TReqTrimRows hydraRequest;
            ToProto(hydraRequest.mutable_tablet_id(), tablet->GetId());
            hydraRequest.set_mount_revision(tablet->GetMountRevision());
            hydraRequest.set_trimmed_row_count(trimmedRowCount);
            return CreateMutation(Slot_->GetHydraManager(), hydraRequest)
                ->Commit()
                .As<void>();
        } catch (const std::exception& ex) {
            return MakeFuture(TError(ex));
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

    TFuture<void> CommitTabletStoresUpdateTransaction(
        TTablet* tablet,
        const ITransactionPtr& transaction)
    {
        LOG_DEBUG("Acquiring tablet stores commit semaphore (TabletId: %v, TransactionId: %v)",
            tablet->GetId(),
            transaction->GetId());

        auto promise = NewPromise<void>();
        tablet
            ->GetStoresUpdateCommitSemaphore()
            ->AsyncAcquire(
                BIND(&TImpl::OnStoresUpdateCommitSemaphoreAcquired, MakeWeak(this), tablet, transaction, promise),
                tablet->GetEpochAutomatonInvoker());
        return promise;
    }

    IYPathServicePtr GetOrchidService()
    {
        return OrchidService_;
    }

    i64 GetDynamicStoresMemoryUsage() const
    {
        return DynamicStoresMemoryTrackerGuard_.GetSize();
    }

    i64 GetStaticStoresMemoryUsage() const
    {
        return StaticStoresMemoryTrackerGuard_.GetSize();
    }

    i64 GetWriteLogsMemoryUsage() const
    {
        return WriteLogsMemoryTrackerGuard_.GetSize();
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet);

private:
    class TOrchidService
        : public TVirtualMapBase
    {
    public:
        static IYPathServicePtr Create(TWeakPtr<TImpl> impl, IInvokerPtr invoker)
        {
            return New<TOrchidService>(std::move(impl))
                ->Via(invoker);
        }

        virtual std::vector<Stroka> GetKeys(i64 limit) const override
        {
            std::vector<Stroka> keys;
            if (auto owner = Owner_.Lock()) {
                for (const auto& tablet : owner->Tablets()) {
                    if (keys.size() >= limit) {
                        break;
                    }
                    keys.push_back(ToString(tablet.first));
                }
            }
            return keys;
        }

        virtual i64 GetSize() const override
        {
            if (auto owner = Owner_.Lock()) {
                return owner->Tablets().size();
            }
            return 0;
        }

        virtual IYPathServicePtr FindItemService(const TStringBuf& key) const override
        {
            if (auto owner = Owner_.Lock()) {
                if (auto tablet = owner->FindTablet(TTabletId::FromString(key))) {
                    auto producer = BIND(&TImpl::BuildTabletOrchidYson, owner, tablet);
                    return ConvertToNode(producer);
                }
            }
            return nullptr;
        }

    private:
        const TWeakPtr<TImpl> Owner_;

        explicit TOrchidService(TWeakPtr<TImpl> impl)
            : Owner_(std::move(impl))
        { }

        DECLARE_NEW_FRIEND();
    };

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

        virtual EPeerState GetAutomatonState() override
        {
            return Owner_->Slot_->GetAutomatonState();
        }

        virtual TColumnEvaluatorCachePtr GetColumnEvaluatorCache() override
        {
            return Owner_->Bootstrap_->GetColumnEvaluatorCache();
        }

        virtual TObjectId GenerateId(EObjectType type) override
        {
            return Owner_->Slot_->GenerateId(type);
        }

        virtual IStorePtr CreateStore(
            TTablet* tablet,
            EStoreType type,
            const TStoreId& storeId,
            const TAddStoreDescriptor* descriptor) override
        {
            return Owner_->CreateStore(tablet, type, storeId, descriptor);
        }

        virtual TTransactionManagerPtr GetTransactionManager() override
        {
            return Owner_->Slot_->GetTransactionManager();
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

    TTabletContext TabletContext_;
    TEntityMap<TTablet, TTabletMapTraits> TabletMap_;
    yhash_set<TTablet*> WaitingForLocksTablets_;

    yhash_set<IDynamicStorePtr> OrphanedStores_;

    TNodeMemoryTrackerGuard DynamicStoresMemoryTrackerGuard_;
    TNodeMemoryTrackerGuard StaticStoresMemoryTrackerGuard_;
    TNodeMemoryTrackerGuard WriteLogsMemoryTrackerGuard_;

    const IYPathServicePtr OrchidService_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(TSaveContext& context) const
    {
        TabletMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        using NYT::Save;

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
            auto storeManager = CreateStoreManager(tablet);
            tablet->SetStoreManager(storeManager);
            auto state = tablet->GetState();
            if (state == ETabletState::UnmountWaitingForLocks ||
                state == ETabletState::FreezeWaitingForLocks)
            {
                YCHECK(WaitingForLocksTablets_.insert(tablet).second);
            }
        }

        const auto& transactionManager = Slot_->GetTransactionManager();
        auto transactions = transactionManager->GetTransactions();
        for (auto* transaction : transactions) {
            YCHECK(!transaction->GetTransient());

            auto handleWriteLog = [&] (const TTransactionWriteLog& writeLog) {
                WriteLogsMemoryTrackerGuard_.UpdateSize(GetTransactionWriteLogMemoryUsage(writeLog));
                int rowCount = 0;
                for (const auto& record : writeLog) {
                    auto* tablet = FindTablet(record.TabletId);
                    if (!tablet) {
                        // NB: Tablet could be missing if it was e.g. forcefully removed.
                        continue;
                    }

                    TWireProtocolReader reader(record.Data);
                    const auto& storeManager = tablet->GetStoreManager();
                    while (!reader.IsFinished()) {
                        storeManager->ExecuteWrite(transaction, &reader, NullTimestamp, false);
                        ++rowCount;
                    }
                }
                return rowCount;
            };
            int immediateRowCount = handleWriteLog(transaction->ImmediateWriteLog());
            int delayedRowCount = handleWriteLog(transaction->DelayedWriteLog());

            LOG_DEBUG_IF(immediateRowCount + delayedRowCount > 0, "Transaction write log applied (TransactionId: %v, "
                "ImmediateRowCount: %v, DelayedRowCount: %v)",
                transaction->GetId(),
                immediateRowCount,
                delayedRowCount);

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
        WaitingForLocksTablets_.clear();
        OrphanedStores_.clear();
        WriteLogsMemoryTrackerGuard_.SetSize(0);
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


    void HydraMountTablet(TReqMountTablet* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto mountRevision = request->mount_revision();
        auto tableId = FromProto<TObjectId>(request->table_id());
        auto schema = FromProto<TTableSchema>(request->schema());
        auto pivotKey = request->has_pivot_key() ? FromProto<TOwningKey>(request->pivot_key()) : TOwningKey();
        auto nextPivotKey = request->has_next_pivot_key() ? FromProto<TOwningKey>(request->next_pivot_key()) : TOwningKey();
        auto mountConfig = DeserializeTableMountConfig((TYsonString(request->mount_config())), tabletId);
        auto readerConfig = DeserializeTabletChunkReaderConfig(TYsonString(request->reader_config()), tabletId);
        auto writerConfig = DeserializeTabletChunkWriterConfig(TYsonString(request->writer_config()), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request->writer_options()), tabletId);
        auto atomicity = EAtomicity(request->atomicity());
        auto commitOrdering = ECommitOrdering(request->commit_ordering());
        auto storeDescriptors = FromProto<std::vector<TAddStoreDescriptor>>(request->stores());
        bool freeze = request->freeze();
        auto replicaDescriptors = FromProto<std::vector<TTableReplicaDescriptor>>(request->replicas());

        auto tabletHolder = std::make_unique<TTablet>(
            mountConfig,
            readerConfig,
            writerConfig,
            writerOptions,
            tabletId,
            mountRevision,
            tableId,
            &TabletContext_,
            schema,
            pivotKey,
            nextPivotKey,
            atomicity,
            commitOrdering);
        auto* tablet = TabletMap_.Insert(tabletId, std::move(tabletHolder));

        if (!tablet->IsPhysicallySorted()) {
            tablet->SetTrimmedRowCount(request->trimmed_row_count());
        }

        auto storeManager = CreateStoreManager(tablet);
        tablet->SetStoreManager(storeManager);

        storeManager->Mount(storeDescriptors);

        tablet->SetState(freeze ? ETabletState::Frozen : ETabletState::Mounted);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet mounted (TabletId: %v, MountRevision: %x, TableId: %v, Keys: %v .. %v, "
            "StoreCount: %v, PartitionCount: %v, TotalRowCount: %v, TrimmedRowCount: %v, Atomicity: %v, "
            "CommitOrdering: %v, Frozen: %v)",
            tabletId,
            mountRevision,
            tableId,
            pivotKey,
            nextPivotKey,
            request->stores_size(),
            tablet->IsPhysicallySorted() ? MakeNullable(tablet->PartitionList().size()) : Null,
            tablet->IsPhysicallySorted() ? Null : MakeNullable(tablet->GetTotalRowCount()),
            tablet->IsPhysicallySorted() ? Null : MakeNullable(tablet->GetTrimmedRowCount()),
            tablet->GetAtomicity(),
            tablet->GetCommitOrdering(),
            freeze);

        for (const auto& descriptor : request->replicas()) {
            AddTableReplica(tablet, descriptor);
        }

        {
            TRspMountTablet response;
            ToProto(response.mutable_tablet_id(), tabletId);
            response.set_frozen(freeze);
            PostMasterMutation(response);
        }

        if (!IsRecovery()) {
            StartTabletEpoch(tablet);
        }
    }

    void HydraUnmountTablet(TReqUnmountTablet* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        if (request->force()) {
            LOG_INFO_UNLESS(IsRecovery(), "Tablet is forcefully unmounted (TabletId: %v)",
                tabletId);

            // Just a formality.
            tablet->SetState(ETabletState::Unmounted);

            for (const auto& pair : tablet->StoreIdMap()) {
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
            // NB: Don't check the result.
            WaitingForLocksTablets_.erase(tablet);
            return;
        }

        auto state = tablet->GetState();
        if (state >= ETabletState::UnmountFirst && state <= ETabletState::UnmountLast) {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to unmount a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "Unmounting tablet (TabletId: %v)",
            tabletId);

        tablet->SetState(ETabletState::UnmountWaitingForLocks);
        // NB: Don't check the result.
        WaitingForLocksTablets_.insert(tablet);

        LOG_INFO_IF(IsLeader(), "Waiting for all tablet locks to be released (TabletId: %v)",
            tabletId);

        if (IsLeader()) {
            CheckIfFullyUnlocked(tablet);
        }
    }

    void HydraRemountTablet(TReqRemountTablet* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountConfig = DeserializeTableMountConfig((TYsonString(request->mount_config())), tabletId);
        auto readerConfig = DeserializeTabletChunkReaderConfig(TYsonString(request->writer_config()), tabletId);
        auto writerConfig = DeserializeTabletChunkWriterConfig(TYsonString(request->writer_config()), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request->writer_options()), tabletId);

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->Remount(mountConfig, readerConfig, writerConfig, writerOptions);

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet remounted (TabletId: %v)",
            tabletId);
    }

    void HydraFreezeTablet(TReqFreezeTablet* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto state = tablet->GetState();
        if (state >= ETabletState::UnmountFirst && state <= ETabletState::UnmountLast ||
            state >= ETabletState::FreezeFirst && state <= ETabletState::FreezeLast)
        {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to freeze a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "Freezing tablet (TabletId: %v)",
            tabletId);

        tablet->SetState(ETabletState::FreezeWaitingForLocks);
        // NB: Don't check the result.
        WaitingForLocksTablets_.insert(tablet);

        LOG_INFO_IF(IsLeader(), "Waiting for all tablet locks to be released (TabletId: %v)",
            tabletId);

        if (IsLeader()) {
            CheckIfFullyUnlocked(tablet);
        }
    }

    void HydraUnfreezeTablet(TReqUnfreezeTablet* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto state = tablet->GetState();
        if (state != ETabletState::Frozen)  {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to unfreeze a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet unfrozen (TabletId: %v)",
            tabletId);

        tablet->SetState(ETabletState::Mounted);

        TRspUnfreezeTablet response;
        ToProto(response.mutable_tablet_id(), tabletId);
        PostMasterMutation(response);
    }

    void HydraSetTabletState(TReqSetTabletState* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto requestedState = ETabletState(request->state());

        switch (requestedState) {
            case ETabletState::FreezeFlushing: {
                auto state = tablet->GetState();
                if (state >= ETabletState::UnmountFirst && state <= ETabletState::UnmountLast) {
                    LOG_INFO_UNLESS(IsRecovery(), "Trying to switch state to %Qv while tablet in %Qlv state, ignored (TabletId: %v)",
                        requestedState,
                        state,
                        tabletId);
                    return;
                }
                // No break intentionaly
            }

            case ETabletState::UnmountFlushing: {
                tablet->SetState(requestedState);

                const auto& storeManager = tablet->GetStoreManager();
                if (requestedState == ETabletState::UnmountFlushing ||
                    tablet->GetActiveStore()->GetRowCount() > 0)
                {
                    storeManager->Rotate(requestedState == ETabletState::FreezeFlushing);
                }

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

                for (const auto& pair : tablet->Replicas()) {
                    const auto& replicaInfo = pair.second;
                    PostTableReplicaStatistics(tablet, replicaInfo);
                }

                TabletMap_.Remove(tabletId);
                // NB: Don't check the result.
                WaitingForLocksTablets_.erase(tablet);

                TRspUnmountTablet response;
                ToProto(response.mutable_tablet_id(), tabletId);
                PostMasterMutation(response);
                break;
            }

            case ETabletState::Frozen: {
                auto state = tablet->GetState();
                if (state >= ETabletState::UnmountFirst && state <= ETabletState::UnmountLast) {
                    LOG_INFO_UNLESS(IsRecovery(), "Trying to switch state to %Qv while tablet in %Qlv state, ignored (TabletId: %v)",
                        requestedState,
                        state,
                        tabletId);
                    return;
                }

                tablet->SetState(ETabletState::Frozen);

                for (const auto& pair : tablet->StoreIdMap()) {
                    if (pair.second->IsChunk()) {
                        pair.second->AsChunk()->SetBackingStore(nullptr);
                    }
                }

                LOG_INFO_UNLESS(IsRecovery(), "Tablet frozen (TabletId: %v)",
                    tabletId);

                // NB: Don't check the result.
                WaitingForLocksTablets_.erase(tablet);

                TRspFreezeTablet response;
                ToProto(response.mutable_tablet_id(), tabletId);
                PostMasterMutation(response);
                break;
            }

            default:
                Y_UNREACHABLE();
        }
    }


    template <class TPrelockedRows>
    void ConfirmRows(TTransaction* transaction, TPrelockedRows& rows, int rowCount)
    {
        for (int index = 0; index < rowCount; ++index) {
            Y_ASSERT(!rows.empty());
            auto rowRef = rows.front();
            rows.pop();
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.StoreManager->ConfirmRow(transaction, rowRef);
            }
        }
    }

    void HydraLeaderExecuteWriteAtomic(
        const TTransactionId& transactionId,
        TTransactionSignature signature,
        int sortedRowCount,
        int orderedRowCount,
        const TTransactionWriteRecord& writeRecord,
        TMutationContext* /*context*/)
    {
        const auto& transactionManager = Slot_->GetTransactionManager();
        auto* transaction = transactionManager->MakeTransactionPersistent(transactionId);

        auto* tablet = FindTablet(writeRecord.TabletId);

        ConfirmRows(transaction, transaction->PrelockedSortedRows(), sortedRowCount);
        ConfirmRows(transaction, transaction->PrelockedOrderedRows(), orderedRowCount);

        bool immediate = !tablet || tablet->GetCommitOrdering() == ECommitOrdering::Weak;
        EnqueueTransactionWriteRecord(transaction, writeRecord, signature, immediate);

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows confirmed (TabletId: %v, TransactionId: %v, "
            "SortedRows: %v, OrderedRows: %v, WriteRecordSize: %v, Immediate: %v)",
            writeRecord.TabletId,
            transactionId,
            sortedRowCount,
            orderedRowCount,
            writeRecord.GetByteSize(),
            immediate);
    }

    void HydraLeaderExecuteWriteNonAtomic(
        const TTabletId& tabletId,
        i64 mountRevision,
        const TTransactionId& transactionId,
        const TSharedRef& recordData,
        TMutationContext* /*context*/)
    {
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            // NB: Tablet could be missing if it was e.g. forcefully removed.
            return;
        }

        tablet->ValidateMountRevision(mountRevision);

        TWireProtocolReader reader(recordData);
        int rowCount = 0;
        const auto& storeManager = tablet->GetStoreManager();
        auto commitTimestamp = TimestampFromTransactionId(transactionId);
        while (!reader.IsFinished()) {
            storeManager->ExecuteWrite(nullptr, &reader, commitTimestamp, false);
            ++rowCount;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows written (TransactionId: %v, TabletId: %v, RowCount: %v, "
            "WriteRecordSize: %v)",
            transactionId,
            tabletId,
            rowCount,
            recordData.Size());
    }

    void HydraFollowerWriteRows(TReqWriteRows* request) noexcept
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto atomicity = AtomicityFromTransactionId(transactionId);
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto signature = request->signature();

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            // NB: Tablet could be missing if it was e.g. forcefully removed.
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto codecId = ECodec(request->codec());
        auto* codec = GetCodec(codecId);
        auto compressedRecordData = TSharedRef::FromString(request->compressed_data());
        auto recordData = codec->Decompress(compressedRecordData);
        TTransactionWriteRecord writeRecord{tabletId, recordData};

        TWireProtocolReader reader(recordData);
        int rowCount = 0;

        const auto& storeManager = tablet->GetStoreManager();

        switch (atomicity) {
            case EAtomicity::Full: {
                const auto& transactionManager = Slot_->GetTransactionManager();
                auto* transaction = transactionManager->GetOrCreateTransaction(
                    transactionId,
                    transactionStartTimestamp,
                    transactionTimeout,
                    false);

                while (!reader.IsFinished()) {
                    storeManager->ExecuteWrite(transaction, &reader, NullTimestamp, false);
                    ++rowCount;
                }

                bool immediate = tablet->GetCommitOrdering() == ECommitOrdering::Weak;
                EnqueueTransactionWriteRecord(transaction, writeRecord, signature, immediate);
            }

            case EAtomicity::None: {
                auto commitTimestamp = TimestampFromTransactionId(transactionId);
                while (!reader.IsFinished()) {
                    storeManager->ExecuteWrite(nullptr, &reader, commitTimestamp, false);
                    ++rowCount;
                }
                break;
            }

            default:
                Y_UNREACHABLE();
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows written (TransactionId: %v, TabletId: %v, RowCount: %v, "
            "WriteRecordSize: %v, Signature: %x)",
            transactionId,
            tabletId,
            rowCount,
            writeRecord.GetByteSize(),
            signature);
    }

    void HydraTrimRows(TReqTrimRows* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto trimmedRowCount = request->trimmed_row_count();

        UpdateTrimmedRowCount(tablet, trimmedRowCount);
    }

    void HydraRotateStore(TReqRotateStore* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }
        if (tablet->GetState() != ETabletState::Mounted) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->Rotate(true);
        UpdateTabletSnapshot(tablet);
    }

    void HydraPrepareUpdateTabletStores(TTransaction* /*transaction*/, TReqUpdateTabletStores* request, bool persistent)
    {
        YCHECK(persistent);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = GetTabletOrThrow(tabletId);

        auto mountRevision = request->mount_revision();
        tablet->ValidateMountRevision(mountRevision);

        TStoreIdList storeIdsToAdd;
        for (const auto& descriptor : request->stores_to_add()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToAdd.push_back(storeId);
        }

        TStoreIdList storeIdsToRemove;
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToRemove.push_back(storeId);
            auto store = tablet->GetStoreOrThrow(storeId);
            auto state = store->GetStoreState();
            if (state != EStoreState::PassiveDynamic && state != EStoreState::Persistent) {
                THROW_ERROR_EXCEPTION("Store %v has invalid state %Qlv",
                    storeId,
                    state);
            }
            store->SetStoreState(EStoreState::RemovePrepared);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet stores update prepared "
            "(TabletId: %v, StoreIdsToAdd: %v, StoreIdsToRemove: %v)",
            tabletId,
            storeIdsToAdd,
            storeIdsToRemove);
    }

    void HydraAbortUpdateTabletStores(TTransaction* /*transaction*/, TReqUpdateTabletStores* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        TStoreIdList storeIdsToAdd;
        for (const auto& descriptor : request->stores_to_add()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToAdd.push_back(storeId);
        }

        TStoreIdList storeIdsToRemove;
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToRemove.push_back(storeId);
        }

        const auto& storeManager = tablet->GetStoreManager();
        for (const auto& storeId : storeIdsToRemove) {
            auto store = tablet->FindStore(storeId);
            if (!store) {
                continue;
            }

            switch (store->GetType()) {
                case EStoreType::SortedDynamic:
                case EStoreType::OrderedDynamic:
                    store->SetStoreState(EStoreState::PassiveDynamic);
                    break;
                case EStoreType::SortedChunk:
                case EStoreType::OrderedChunk:
                    store->SetStoreState(EStoreState::Persistent);
                    break;
                default:
                    Y_UNREACHABLE();
            }

            if (IsLeader()) {
                storeManager->BackoffStoreRemoval(store);
            }
        }

        if (IsLeader()) {
            CheckIfFullyFlushed(tablet);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet stores update aborted "
            "(TabletId: %v, StoreIdsToAdd: %v, StoreIdsToRemove: %v)",
            tabletId,
            storeIdsToAdd,
            storeIdsToRemove);
    }

    void HydraCommitUpdateTabletStores(TTransaction* /*transaction*/, TReqUpdateTabletStores* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();

        auto mountConfig = tablet->GetConfig();
        auto inMemoryManager = Bootstrap_->GetInMemoryManager();

        // NB: Must handle store removals before store additions since
        // row index map forbids having multiple stores with the same starting row index.
        // But before proceeding to removals, we must take care of backing stores.
        yhash_map<TStoreId, IDynamicStorePtr> idToBackingStore;
        auto registerBackingStore = [&] (const IStorePtr& store) {
            YCHECK(idToBackingStore.insert(std::make_pair(store->GetId(), store->AsDynamic())).second);
        };
        auto getBackingStore = [&] (const TStoreId& id) {
            auto it = idToBackingStore.find(id);
            YCHECK(it != idToBackingStore.end());
            return it->second;
        };

        if (!IsRecovery()) {
            for (const auto& descriptor : request->stores_to_add()) {
                if (descriptor.has_backing_store_id()) {
                    auto backingStoreId = FromProto<TStoreId>(descriptor.backing_store_id());
                    auto backingStore = tablet->GetStore(backingStoreId);
                    registerBackingStore(backingStore);
                }
            }
        }

        std::vector<TStoreId> removedStoreIds;
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            removedStoreIds.push_back(storeId);

            auto store = tablet->GetStore(storeId);
            storeManager->RemoveStore(store);

            LOG_DEBUG_UNLESS(IsRecovery(), "Store removed (TabletId: %v, StoreId: %v)",
                tabletId,
                storeId);
        }

        std::vector<TStoreId> addedStoreIds;
        for (const auto& descriptor : request->stores_to_add()) {
            auto storeType = EStoreType(descriptor.store_type());
            auto storeId = FromProto<TChunkId>(descriptor.store_id());
            addedStoreIds.push_back(storeId);

            auto store = CreateStore(tablet, storeType, storeId, &descriptor)->AsChunk();
            storeManager->AddStore(store, false);

            TStoreId backingStoreId;
            if (!IsRecovery() && descriptor.has_backing_store_id()) {
                backingStoreId = FromProto<TStoreId>(descriptor.backing_store_id());
                auto backingStore = getBackingStore(backingStoreId);
                SetBackingStore(tablet, store, backingStore);
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Store added (TabletId: %v, StoreId: %v, MaxTimestamp: %v, BackingStoreId: %v)",
                tabletId,
                storeId,
                store->GetMaxTimestamp(),
                backingStoreId);
        }

        auto retainedTimestamp = std::max(
            tablet->GetRetainedTimestamp(),
            static_cast<TTimestamp>(request->retained_timestamp()));
        tablet->SetRetainedTimestamp(retainedTimestamp);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet stores update committed "
            "(TabletId: %v, AddedStoreIds: %v, RemovedStoreIds: %v, RetainedTimestamp: %v)",
            tabletId,
            addedStoreIds,
            removedStoreIds,
            retainedTimestamp);

        UpdateTabletSnapshot(tablet);

        if (IsLeader()) {
            CheckIfFullyFlushed(tablet);
        }
    }

    void HydraSplitPartition(TReqSplitPartition* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        YCHECK(tablet->IsPhysicallySorted());

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto partitionId = FromProto<TPartitionId>(request->partition_id());
        auto* partition = tablet->GetPartition(partitionId);

        auto pivotKeys = FromProto<std::vector<TOwningKey>>(request->pivot_keys());

        int partitionIndex = partition->GetIndex();
        i64 partitionDataSize = partition->GetUncompressedDataSize();

        auto storeManager = tablet->GetStoreManager()->AsSorted();
        bool result = storeManager->SplitPartition(partition->GetIndex(), pivotKeys);
        if (!result) {
            LOG_INFO_UNLESS(IsRecovery(), "Partition split failed (TabletId: %v, PartitionId: %v, Keys: %v)",
                tablet->GetId(),
                partitionId,
                JoinToString(pivotKeys, STRINGBUF(" .. ")));
            return;
        }

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Partition split (TabletId: %v, OriginalPartitionId: %v, "
            "ResultingPartitionIds: %v, DataSize: %v, Keys: %v)",
            tablet->GetId(),
            partitionId,
            MakeFormattableRange(
                MakeRange(
                    tablet->PartitionList().data() + partitionIndex,
                    tablet->PartitionList().data() + partitionIndex + pivotKeys.size()),
                TPartitionIdFormatter()),
            partitionDataSize,
            JoinToString(pivotKeys, STRINGBUF(" .. ")));
    }

    void HydraMergePartitions(TReqMergePartitions* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        YCHECK(tablet->IsPhysicallySorted());

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto firstPartitionId = FromProto<TPartitionId>(request->partition_id());
        auto* firstPartition = tablet->GetPartition(firstPartitionId);

        int firstPartitionIndex = firstPartition->GetIndex();
        int lastPartitionIndex = firstPartitionIndex + request->partition_count() - 1;

        auto originalPartitionIds = Format("%v",
            MakeFormattableRange(
                MakeRange(
                    tablet->PartitionList().data() + firstPartitionIndex,
                    tablet->PartitionList().data() + lastPartitionIndex + 1),
                TPartitionIdFormatter()));

        i64 partitionsDataSize = 0;
        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            const auto& partition = tablet->PartitionList()[index];
            partitionsDataSize += partition->GetUncompressedDataSize();
        }

        auto storeManager = tablet->GetStoreManager()->AsSorted();
        storeManager->MergePartitions(
            firstPartition->GetIndex(),
            firstPartition->GetIndex() + request->partition_count() - 1);

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Partitions merged (TabletId: %v, OriginalPartitionIds: %v, "
            "ResultingPartitionId: %v, DataSize: %v)",
            tablet->GetId(),
            originalPartitionIds,
            tablet->PartitionList()[firstPartitionIndex]->GetId(),
            partitionsDataSize);
    }

    void HydraUpdatePartitionSampleKeys(TReqUpdatePartitionSampleKeys* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        YCHECK(tablet->IsPhysicallySorted());

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            return;
        }

        auto partitionId = FromProto<TPartitionId>(request->partition_id());
        auto* partition = tablet->FindPartition(partitionId);
        if (!partition) {
            return;
        }

        TWireProtocolReader reader(TSharedRef::FromString(request->sample_keys()));
        auto sampleKeys = reader.ReadUnversionedRowset(true);

        auto storeManager = tablet->GetStoreManager()->AsSorted();
        storeManager->UpdatePartitionSampleKeys(partition, sampleKeys);

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Partition sample keys updated (TabletId: %v, PartitionId: %v, SampleKeyCount: %v)",
            tabletId,
            partition->GetId(),
            sampleKeys.Size());
    }

    void HydraAddTableReplica(TReqAddTableReplica* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        AddTableReplica(tablet, request->replica());
    }

    void HydraRemoveTableReplica(TReqRemoveTableReplica* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        RemoveTableReplica(tablet, replicaId);
    }

    void HydraEnableTableReplica(TReqEnableTableReplica* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replicaInfo = tablet->FindReplicaInfo(replicaId);
        if (!replicaInfo) {
            return;
        }

        EnableTableReplica(tablet, replicaInfo);
    }

    void HydraDisableTableReplica(TReqDisableTableReplica* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replicaInfo = tablet->FindReplicaInfo(replicaId);
        if (!replicaInfo) {
            return;
        }

        DisableTableReplica(tablet, replicaInfo);
    }

    void HydraPrepareReplicateRows(TTransaction* transaction, TReqReplicateRows* request, bool persistent)
    {
        YCHECK(persistent);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = GetTabletOrThrow(tabletId);

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replicaInfo = tablet->GetReplicaInfoOrThrow(replicaId);

        if (replicaInfo->GetState() != ETableReplicaState::Enabled) {
            THROW_ERROR_EXCEPTION("Replica %v is not enabled",
                replicaId);
        }

        if (replicaInfo->GetPreparedReplicationTransactionId()) {
            THROW_ERROR_EXCEPTION("Cannot prepare rows for replica %v of tablet %v by transaction %v since these are already "
                "prepared by transaction %v",
                transaction->GetId(),
                replicaId,
                tabletId,
                replicaInfo->GetPreparedReplicationTransactionId());
        }

        YCHECK(replicaInfo->GetPreparedReplicationRowIndex() == -1);
        replicaInfo->SetPreparedReplicationRowIndex(request->new_replication_row_index());
        replicaInfo->SetPreparedReplicationTransactionId(transaction->GetId());

        LOG_DEBUG_UNLESS(IsRecovery(), "Replicated rows prepared (TabletId: %v, ReplicaId: %v, TransactionId: %v, "
            "CurrentReplicationRowIndex: %v->%v, CurrentReplicationTimestamp: %v->%v)",
            tabletId,
            replicaId,
            transaction->GetId(),
            replicaInfo->GetCurrentReplicationRowIndex(),
            request->new_replication_row_index(),
            replicaInfo->GetCurrentReplicationTimestamp(),
            request->new_replication_timestamp());

    }

    void HydraCommitReplicateRows(TTransaction* transaction, TReqReplicateRows* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replicaInfo = tablet->FindReplicaInfo(replicaId);
        if (!replicaInfo) {
            return;
        }

        YCHECK(replicaInfo->GetPreparedReplicationRowIndex() == request->new_replication_row_index());
        YCHECK(replicaInfo->GetPreparedReplicationTransactionId() == transaction->GetId());
        replicaInfo->SetPreparedReplicationRowIndex(-1);
        replicaInfo->SetPreparedReplicationTransactionId(NullTransactionId);

        auto prevCurrentReplicationRowIndex = replicaInfo->GetCurrentReplicationRowIndex();
        auto prevCurrentReplicationTimestamp = replicaInfo->GetCurrentReplicationTimestamp();
        auto prevTrimmedRowCount = tablet->GetTrimmedRowCount();

        auto newCurrentReplicationRowIndex = request->new_replication_row_index();
        auto newCurrentReplicationTimestamp = request->new_replication_timestamp();

        YCHECK(newCurrentReplicationRowIndex >= prevCurrentReplicationRowIndex);
        YCHECK(newCurrentReplicationTimestamp >= prevCurrentReplicationTimestamp);

        replicaInfo->SetCurrentReplicationRowIndex(newCurrentReplicationRowIndex);
        replicaInfo->SetCurrentReplicationTimestamp(newCurrentReplicationTimestamp);

        AdvanceReplicatedTrimmedRowCount(transaction, tablet);

        LOG_DEBUG_UNLESS(IsRecovery(), "Replicated rows committed (TabletId: %v, ReplicaId: %v, TransactionId: %v, "
            "CurrentReplicationRowIndex: %v->%v, CurrentReplicationTimestamp: %v->%v, TrimmedRowCount: %v->%v)",
            tabletId,
            replicaId,
            transaction->GetId(),
            prevCurrentReplicationRowIndex,
            replicaInfo->GetCurrentReplicationRowIndex(),
            prevCurrentReplicationTimestamp,
            replicaInfo->GetCurrentReplicationTimestamp(),
            prevTrimmedRowCount,
            tablet->GetTrimmedRowCount());
    }

    void HydraAbortReplicateRows(TTransaction* transaction, TReqReplicateRows* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replicaInfo = tablet->FindReplicaInfo(replicaId);
        if (!replicaInfo) {
            return;
        }

        if (transaction->GetId() != replicaInfo->GetPreparedReplicationTransactionId()) {
            return;
        }

        replicaInfo->SetPreparedReplicationRowIndex(-1);
        replicaInfo->SetPreparedReplicationTransactionId(NullTransactionId);

        LOG_DEBUG_UNLESS(IsRecovery(), "Replicated rows aborted (TabletId: %v, ReplicaId: %v, TransactionId: %v, "
            "CurrentReplicationRowIndex: %v->%v, CurrentReplicationTimestamp: %v->%v)",
            tabletId,
            replicaId,
            transaction->GetId(),
            replicaInfo->GetCurrentReplicationRowIndex(),
            request->new_replication_row_index(),
            replicaInfo->GetCurrentReplicationTimestamp(),
            request->new_replication_timestamp());
    }


    template <class TRef>
    void PrepareRow(TTransaction* transaction, const TRef& rowRef)
    {
        // NB: Don't call ValidateAndDiscardRowRef, row refs are just scanned.
        if (ValidateRowRef(rowRef)) {
            rowRef.StoreManager->PrepareRow(transaction, rowRef);
        }
    }

    template <class TLockedRows, class TPrelockedRows>
    void PrepareRows(TTransaction* transaction, TLockedRows& lockedRows, TPrelockedRows& prelockedRows)
    {
        for (const auto& rowRef : lockedRows) {
            PrepareRow(transaction, rowRef);
        }

        for (auto it = prelockedRows.begin();
             it != prelockedRows.end();
             prelockedRows.move_forward(it))
        {
            PrepareRow(transaction, *it);
        }
    }

    void OnTransactionPrepared(TTransaction* transaction)
    {
        auto lockedSortedRowCount = transaction->LockedSortedRows().size();
        auto prelockedSortedRowCount = transaction->PrelockedSortedRows().size();
        auto lockedOrderedRowCount = transaction->LockedOrderedRows().size();
        auto prelockedOrderedRowCount = transaction->PrelockedOrderedRows().size();

        PrepareRows(transaction, transaction->LockedSortedRows(), transaction->PrelockedSortedRows());
        PrepareRows(transaction, transaction->LockedOrderedRows(), transaction->PrelockedOrderedRows());

        LOG_DEBUG_UNLESS(IsRecovery() || (lockedSortedRowCount + lockedOrderedRowCount == 0),
            "Locked rows prepared (TransactionId: %v, "
            "SortedLockedRows: %v, SortedPrelockedRows: %v, "
            "OrderedLockedRows: %v, OrderedPrelockedRows: %v)",
            transaction->GetId(),
            lockedSortedRowCount,
            prelockedSortedRowCount,
            lockedOrderedRowCount,
            prelockedOrderedRowCount);
    }


    template <class TLockedRows, class TPrelockedRows>
    int CommitRows(TTransaction* transaction, TLockedRows& lockedRows, TPrelockedRows& prelockedRows, bool immediate)
    {
        YCHECK(prelockedRows.empty());
        auto it = lockedRows.begin();
        auto jt = it;
        int count = 0;
        while (it != lockedRows.end()) {
            const auto& rowRef = *it++;
            if (!ValidateAndDiscardRowRef(rowRef)) {
                continue;
            }
            if (rowRef.Immediate != immediate) {
                *jt++ = rowRef;
                continue;
            }
            ++count;
            rowRef.StoreManager->CommitRow(transaction, rowRef);
        }
        lockedRows.erase(jt, lockedRows.end());
        return count;
    }

    void OnTransactionCommitted(TTransaction* transaction)
    {
        int sortedRowCount = CommitRows(
            transaction,
            transaction->LockedSortedRows(),
            transaction->PrelockedSortedRows(),
            true);
        int orderedRowCount = CommitRows(
            transaction,
            transaction->LockedOrderedRows(),
            transaction->PrelockedOrderedRows(),
            true);

        YCHECK(transaction->LockedSortedRows().empty());

        ClearTransactionWriteLog(&transaction->ImmediateWriteLog());

        LOG_DEBUG_UNLESS(IsRecovery() || (sortedRowCount + orderedRowCount == 0),
            "Immediate locked rows committed (TransactionId: %v, SortedRows: %v, OrderedRows: %v)",
            transaction->GetId(),
            sortedRowCount,
            orderedRowCount);

        OnTransactionFinished(transaction);
    }

    void OnTransactionSerialized(TTransaction* transaction)
    {
        int orderedRowCount = CommitRows(
            transaction,
            transaction->LockedOrderedRows(),
            transaction->PrelockedOrderedRows(),
            false);

        YCHECK(transaction->LockedSortedRows().empty());
        YCHECK(transaction->LockedOrderedRows().empty());

        ClearTransactionWriteLog(&transaction->DelayedWriteLog());

        LOG_DEBUG_UNLESS(IsRecovery() || orderedRowCount == 0,
            "Delayed locked rows committed (TransactionId: %v, OrderedRows: %v)",
            transaction->GetId(),
            orderedRowCount);

        OnTransactionFinished(transaction);
    }


    template <class TLockedRows, class TPrelockedRows>
    void AbortRows(TTransaction* transaction, TLockedRows& lockedRows, TPrelockedRows& prelockedRows)
    {
        YCHECK(prelockedRows.empty());
        for (const auto& rowRef : lockedRows) {
            if (ValidateAndDiscardRowRef(rowRef)) {
               rowRef.StoreManager->AbortRow(transaction, rowRef);
            }
        }
        lockedRows.clear();
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        auto lockedSortedRowCount = transaction->LockedSortedRows().size();
        auto lockedOrderedRowCount = transaction->LockedOrderedRows().size();

        AbortRows(transaction, transaction->LockedSortedRows(), transaction->PrelockedSortedRows());
        AbortRows(transaction, transaction->LockedOrderedRows(), transaction->PrelockedOrderedRows());

        ClearTransactionWriteLog(&transaction->ImmediateWriteLog());
        ClearTransactionWriteLog(&transaction->DelayedWriteLog());

        LOG_DEBUG_UNLESS(IsRecovery() || (lockedSortedRowCount + lockedOrderedRowCount == 0),
            "Locked rows aborted (TransactionId: %v, SortedRows: %v, OrderedRows: %v)",
            transaction->GetId(),
            lockedSortedRowCount,
            lockedOrderedRowCount);

        OnTransactionFinished(transaction);
    }

    template <class TPrelockedRows>
    void TransientResetRows(TTransaction* transaction, TPrelockedRows& rows)
    {
        while (!rows.empty()) {
            auto rowRef = rows.front();
            rows.pop();
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.StoreManager->AbortRow(transaction, rowRef);
            }
        }
    }

    void OnTransactionTransientReset(TTransaction* transaction)
    {
        TransientResetRows(transaction, transaction->PrelockedSortedRows());
        TransientResetRows(transaction, transaction->PrelockedOrderedRows());
    }

    void OnTransactionFinished(TTransaction* /*transaction*/)
    {
        if (IsLeader()) {
            for (auto* tablet : WaitingForLocksTablets_) {
                CheckIfFullyUnlocked(tablet);
            }
        }
    }


    static i64 GetTransactionWriteLogMemoryUsage(const TTransactionWriteLog& writeLog)
    {
        i64 result = 0;
        for (const auto& record : writeLog) {
            result += record.GetByteSize();
        }
        return result;
    }

    void EnqueueTransactionWriteRecord(
        TTransaction* transaction,
        const TTransactionWriteRecord& record,
        TTransactionSignature signature,
        bool immediate)
    {
        WriteLogsMemoryTrackerGuard_.UpdateSize(record.GetByteSize());
        auto& writeLog = immediate ? transaction->ImmediateWriteLog() : transaction->DelayedWriteLog();
        writeLog.Enqueue(record);
        transaction->SetPersistentSignature(transaction->GetPersistentSignature() + signature);
    }

    void ClearTransactionWriteLog(TTransactionWriteLog* writeLog)
    {
        WriteLogsMemoryTrackerGuard_.UpdateSize(-GetTransactionWriteLogMemoryUsage(*writeLog));
        writeLog->Clear();
    }


    void SetStoreOrphaned(TTablet* tablet, IStorePtr store)
    {
        if (store->GetStoreState() == EStoreState::Orphaned) {
            return;
        }

        store->SetStoreState(EStoreState::Orphaned);

        if (!store->IsDynamic()) {
            return;
        }
        
        auto dynamicStore = store->AsDynamic();
        auto lockCount = dynamicStore->GetLockCount();
        if (lockCount > 0) {
            YCHECK(OrphanedStores_.insert(dynamicStore).second);
            LOG_INFO_UNLESS(IsRecovery(), "Dynamic memory store is orphaned and will be kept "
                "(StoreId: %v, TabletId: %v, LockCount: %v)",
                store->GetId(),
                tablet->GetId(),
                lockCount);
        }
    }


    template <class TRowRef>
    bool ValidateRowRef(const TRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        return store->GetStoreState() != EStoreState::Orphaned;
    }

    template <class TRowRef>
    bool ValidateAndDiscardRowRef(const TRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        if (store->GetStoreState() != EStoreState::Orphaned) {
            return true;
        }

        auto lockCount = store->Unlock();
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

            case EWireProtocolCommand::VersionedLookupRows:
                VersionedLookupRows(
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
        TTimestamp transactionStartTimestamp,
        TDuration transactionTimeout,
        TTransactionSignature signature,
        TWireProtocolReader* reader,
        TFuture<void>* commitResult)
    {
        const auto& tabletId = tablet->GetId();
        const auto& storeManager = tablet->GetStoreManager();

        const auto& transactionManager = Slot_->GetTransactionManager();
        bool transactionIsFresh;
        auto* transaction = transactionManager->GetOrCreateTransaction(
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            true,
            &transactionIsFresh);
        ValidateTransactionActive(transaction);

        auto prelockedSortedBefore = transaction->PrelockedSortedRows().size();
        auto prelockedOrderedBefore = transaction->PrelockedOrderedRows().size();
        auto readerBegin = reader->GetCurrent();

        TError error;
        TNullable<TRowBlockedException> rowBlockedEx;

        while (!reader->IsFinished()) {
            const char* readerCheckpoint = reader->GetCurrent();
            auto rewindReader = [&] () {
                reader->SetCurrent(readerCheckpoint);
            };
            try {
                storeManager->ExecuteWrite(transaction, reader, NullTimestamp, true);
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

        auto prelockedSortedAfter = transaction->PrelockedSortedRows().size();
        auto prelockedOrderedAfter = transaction->PrelockedOrderedRows().size();

        auto prelockedSortedDelta = prelockedSortedAfter - prelockedSortedBefore;
        auto prelockedOrderedDelta = prelockedOrderedAfter - prelockedOrderedBefore;

        if (prelockedSortedDelta + prelockedOrderedDelta > 0) {
            auto adjustedSignature = reader->IsFinished() ? signature : 0;
            LOG_DEBUG("Rows prelocked (TransactionId: %v, TabletId: %v, SortedRows: %v, OrderedRows: %v, "
                "Signature: %x)",
                transactionId,
                tabletId,
                prelockedSortedDelta,
                prelockedOrderedDelta,
                adjustedSignature);

            transaction->SetTransientSignature(transaction->GetTransientSignature() + adjustedSignature);

            auto readerEnd = reader->GetCurrent();
            auto recordData = reader->Slice(readerBegin, readerEnd);
            auto compressedRecordData = ChangelogCodec_->Compress(recordData);
            auto writeRecord = TTransactionWriteRecord{tabletId, recordData};

            TReqWriteRows hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            hydraRequest.set_transaction_start_timestamp(transactionStartTimestamp);
            hydraRequest.set_transaction_timeout(ToProto(transactionTimeout));
            ToProto(hydraRequest.mutable_tablet_id(), tabletId);
            hydraRequest.set_mount_revision(tablet->GetMountRevision());
            hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
            hydraRequest.set_compressed_data(ToString(compressedRecordData));
            hydraRequest.set_signature(adjustedSignature);
            *commitResult = CreateMutation(Slot_->GetHydraManager(), hydraRequest)
                ->SetHandler(BIND(
                    &TImpl::HydraLeaderExecuteWriteAtomic,
                    MakeStrong(this),
                    transactionId,
                    adjustedSignature,
                    prelockedSortedDelta,
                    prelockedOrderedDelta,
                    writeRecord))
                ->Commit()
                 .As<void>();
        } else if (transactionIsFresh) {
            transactionManager->DropTransaction(transaction);
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

        TReqWriteRows hydraRequest;
        ToProto(hydraRequest.mutable_transaction_id(), transactionId);
        ToProto(hydraRequest.mutable_tablet_id(), tablet->GetId());
        hydraRequest.set_mount_revision(tablet->GetMountRevision());
        hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
        hydraRequest.set_compressed_data(ToString(compressedRecordData));
        *commitResult = CreateMutation(Slot_->GetHydraManager(), hydraRequest)
            ->SetHandler(BIND(
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
        auto state = tablet->GetState();
        if (state != ETabletState::UnmountWaitingForLocks && state != ETabletState::FreezeWaitingForLocks) {
            return;
        }

        if (tablet->GetStoreManager()->HasActiveLocks()) {
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "All tablet locks released (TabletId: %v)",
            tablet->GetId());

        ETabletState newTransientState;
        ETabletState newPersistentState;
        switch (state) {
            case ETabletState::UnmountWaitingForLocks:
                newTransientState = ETabletState::UnmountFlushPending;
                newPersistentState = ETabletState::UnmountFlushing;
                break;
            case ETabletState::FreezeWaitingForLocks:
                newTransientState = ETabletState::FreezeFlushPending;
                newPersistentState = ETabletState::FreezeFlushing;
                break;
            default:
                Y_UNREACHABLE();
        }
        tablet->SetState(newTransientState);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        request.set_state(static_cast<int>(newPersistentState));
        CommitTabletMutation(request);
    }

    void CheckIfFullyFlushed(TTablet* tablet)
    {
        auto state = tablet->GetState();
        if (state != ETabletState::UnmountFlushing && state != ETabletState::FreezeFlushing) {
            return;
        }

        if (tablet->GetStoreManager()->HasUnflushedStores()) {
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "All tablet stores flushed (TabletId: %v)",
            tablet->GetId());

        ETabletState newTransientState;
        ETabletState newPersistentState;
        switch (state) {
            case ETabletState::UnmountFlushing:
                newTransientState = ETabletState::UnmountPending;
                newPersistentState = ETabletState::Unmounted;
                break;
            case ETabletState::FreezeFlushing:
                newTransientState = ETabletState::FreezePending;
                newPersistentState = ETabletState::Frozen;
                break;
            default:
                Y_UNREACHABLE();
        }
        tablet->SetState(newTransientState);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        request.set_state(static_cast<int>(newPersistentState));
        CommitTabletMutation(request);
    }


    void CommitTabletMutation(const ::google::protobuf::MessageLite& message)
    {
        auto mutation = CreateMutation(Slot_->GetHydraManager(), message);
        Slot_->GetEpochAutomatonInvoker()->Invoke(
            BIND(IgnoreResult(&TMutation::CommitAndLog), mutation, Logger));
    }

    void PostMasterMutation(const ::google::protobuf::MessageLite& message)
    {
        const auto& hiveManager = Slot_->GetHiveManager();
        hiveManager->PostMessage(Slot_->GetMasterMailbox(), message);
    }


    void StartTabletEpoch(TTablet* tablet)
    {
        const auto& storeManager = tablet->GetStoreManager();
        storeManager->StartEpoch(Slot_);

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->RegisterTabletSnapshot(Slot_, tablet);

        for (auto& pair : tablet->Replicas()) {
            auto& replicaInfo = pair.second;
            StartTableReplicaEpoch(tablet, &replicaInfo);
        }
    }

    void StopTabletEpoch(TTablet* tablet)
    {
        const auto& storeManager = tablet->GetStoreManager();
        if (storeManager) {
            // Store Manager could be null if snapshot loading is aborted.
            storeManager->StopEpoch();
        }

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->UnregisterTabletSnapshot(Slot_, tablet);

        for (auto& pair : tablet->Replicas()) {
            auto& replicaInfo = pair.second;
            StopTableReplicaEpoch(&replicaInfo);
        }
    }


    void StartTableReplicaEpoch(TTablet* tablet, TTableReplicaInfo* replicaInfo)
    {
        replicaInfo->SetReplicator(New<TTableReplicator>(
            Config_,
            tablet,
            replicaInfo,
            Bootstrap_->GetClusterDirectory(),
            Bootstrap_->GetMasterClient()->GetNativeConnection(),
            Slot_,
            Bootstrap_->GetTabletSlotManager(),
            CreateSerializedInvoker(Bootstrap_->GetTableReplicatorPoolInvoker())));

        if (replicaInfo->GetState() == ETableReplicaState::Enabled) {
            replicaInfo->GetReplicator()->Enable();
        }
    }

    void StopTableReplicaEpoch(TTableReplicaInfo* replicaInfo)
    {
        if (replicaInfo->GetReplicator()) {
            replicaInfo->GetReplicator()->Disable();
        }
        replicaInfo->SetReplicator(nullptr);
    }


    void SetBackingStore(TTablet* tablet, IChunkStorePtr store, IDynamicStorePtr backingStore)
    {
        store->SetBackingStore(backingStore);
        LOG_DEBUG("Backing store set (StoreId: %v, BackingStoreId: %v)",
            store->GetId(),
            backingStore->GetId());

        TDelayedExecutor::Submit(
            // NB: Submit the callback via the regular automaton invoker, not the epoch one since
            // we need the store to be released even if the epoch ends.
            BIND(&TTabletManager::TImpl::ReleaseBackingStore, MakeWeak(this), MakeWeak(store))
                .Via(Slot_->GetAutomatonInvoker()),
            tablet->GetConfig()->BackingStoreRetentionTime);
    }

    void ReleaseBackingStore(TWeakPtr<IChunkStore> storeWeak)
    {
        auto store = storeWeak.Lock();
        if (!store) {
            return;
        }
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        store->SetBackingStore(nullptr);
        LOG_DEBUG("Backing store released (StoreId: %v)", store->GetId());
    }

    void BuildTabletOrchidYson(TTablet* tablet, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("table_id").Value(tablet->GetTableId())
                .Item("state").Value(tablet->GetState())
                .Item("config")
                    .BeginAttributes()
                        .Item("opaque").Value(true)
                    .EndAttributes()
                    .Value(tablet->GetConfig())
                .DoIf(
                    tablet->IsPhysicallySorted(), [&] (TFluentMap fluent) {
                    fluent
                        .Item("pivot_key").Value(tablet->GetPivotKey())
                        .Item("next_pivot_key").Value(tablet->GetNextPivotKey())
                        .Item("eden").Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), tablet->GetEden()))
                        .Item("partitions").DoListFor(
                            tablet->PartitionList(), [&] (TFluentList fluent, const std::unique_ptr<TPartition>& partition) {
                                fluent
                                    .Item()
                                    .Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), partition.get()));
                            });
                })
                .DoIf(!tablet->IsPhysicallySorted(), [&] (TFluentMap fluent) {
                    fluent
                        .Item("stores").DoMapFor(
                            tablet->StoreIdMap(),
                            [&] (TFluentMap fluent, const std::pair<const TStoreId, IStorePtr>& pair) {
                                const auto& store = pair.second;
                                fluent
                                    .Item(ToString(store->GetId()))
                                    .Do(BIND(&TImpl::BuildStoreOrchidYson, Unretained(this), store));
                            })
                        .Item("trimmed_row_count").Value(tablet->GetTrimmedRowCount());
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
                .Item("sample_key_count").Value(partition->GetSampleKeys()->Keys.Size())
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


    TNodeMemoryTrackerGuard* GetMemoryTrackerGuardFromStoreType(EStoreType type)
    {
        switch (type) {
            case EStoreType::SortedDynamic:
            case EStoreType::OrderedDynamic:
                return &DynamicStoresMemoryTrackerGuard_;
            case EStoreType::SortedChunk:
            case EStoreType::OrderedChunk:
                return &StaticStoresMemoryTrackerGuard_;
            default:
                Y_UNREACHABLE();
        }
    }

    void OnStoreMemoryUsageUpdated(EStoreType type, i64 delta)
    {
        auto* guard = GetMemoryTrackerGuardFromStoreType(type);
        guard->UpdateSize(delta);
    }

    void StartMemoryUsageTracking(IStorePtr store)
    {
        store->SubscribeMemoryUsageUpdated(BIND(
            &TImpl::OnStoreMemoryUsageUpdated,
            MakeWeak(this),
            store->GetType()));
    }

    void ValidateMemoryLimit()
    {
        if (Bootstrap_->GetTabletSlotManager()->IsOutOfMemory()) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Node is out of tablet memory, all writes disabled");
        }
    }

    void ValidateClientTimestamp(const TTransactionId& transactionId)
    {
        auto clientTimestamp = TimestampFromTransactionId(transactionId);
        auto timestampProvider = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetTimestampProvider();
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
        auto storeCount = tablet->StoreIdMap().size();
        auto storeLimit = tablet->GetConfig()->MaxStoresPerTablet;
        if (storeCount >= storeLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("store_count", storeCount)
                << TErrorAttribute("store_limit", storeLimit);
        }

        auto overlappingStoreCount = tablet->GetOverlappingStoreCount();
        auto overlappingStoreLimit = tablet->GetConfig()->MaxOverlappingStoreCount;
        if (overlappingStoreCount >= overlappingStoreLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many overlapping stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("overlapping_store_count", overlappingStoreCount)
                << TErrorAttribute("overlapping_store_limit", overlappingStoreLimit);
        }
    }


    void UpdateTabletSnapshot(TTablet* tablet)
    {
        if (!IsRecovery()) {
            auto slotManager = Bootstrap_->GetTabletSlotManager();
            slotManager->RegisterTabletSnapshot(Slot_, tablet);
        }
    }

    void ValidateTabletMounted(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (tablet->GetState() != ETabletState::Mounted) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::TabletNotMounted,
                "Tablet %v is not in \"mounted\" state",
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

    TTabletChunkReaderConfigPtr DeserializeTabletChunkReaderConfig(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTabletChunkReaderConfigPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing reader config (TabletId: %v)",
                 tabletId);
            return New<TTabletChunkReaderConfig>();
        }
    }

    TTabletChunkWriterConfigPtr DeserializeTabletChunkWriterConfig(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTabletChunkWriterConfigPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing writer config (TabletId: %v)",
                 tabletId);
            return New<TTabletChunkWriterConfig>();
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


    IStoreManagerPtr CreateStoreManager(TTablet* tablet)
    {
        if (tablet->IsReplicated()) {
            if (tablet->TableSchema().IsSorted()) {
                return DoCreateStoreManager<TReplicatedStoreManager>(tablet);
            } else {
                Y_UNREACHABLE();
            }
        } else {
            if (tablet->IsPhysicallySorted()) {
                return DoCreateStoreManager<TSortedStoreManager>(tablet);
            } else {
                return DoCreateStoreManager<TOrderedStoreManager>(tablet);
            }
        }
    }

    template <class TImpl>
    IStoreManagerPtr DoCreateStoreManager(TTablet* tablet)
    {
        return New<TImpl>(
            Config_,
            tablet,
            &TabletContext_,
            Slot_->GetHydraManager(),
            Bootstrap_->GetInMemoryManager(),
            Bootstrap_->GetMasterClient());
    }


    IStorePtr CreateStore(
        TTablet* tablet,
        EStoreType type,
        const TStoreId& storeId,
        const TAddStoreDescriptor* descriptor)
    {
        auto store = DoCreateStore(tablet, type, storeId, descriptor);
        StartMemoryUsageTracking(store);
        return store;
    }

    IStorePtr DoCreateStore(
        TTablet* tablet,
        EStoreType type,
        const TStoreId& storeId,
        const TAddStoreDescriptor* descriptor)
    {
        switch (type) {
            case EStoreType::SortedChunk: {
                auto store = New<TSortedChunkStore>(
                    Config_,
                    storeId,
                    tablet,
                    Bootstrap_->GetBlockCache(),
                    Bootstrap_->GetChunkRegistry(),
                    Bootstrap_->GetChunkBlockManager(),
                    Bootstrap_->GetMasterClient(),
                    Bootstrap_->GetMasterConnector()->GetLocalDescriptor());
                store->Initialize(descriptor);
                return store;
            }

            case EStoreType::SortedDynamic:
                return New<TSortedDynamicStore>(
                    Config_,
                    storeId,
                    tablet);

            case EStoreType::OrderedChunk: {
                auto store = New<TOrderedChunkStore>(
                    Config_,
                    storeId,
                    tablet,
                    Bootstrap_->GetBlockCache(),
                    Bootstrap_->GetChunkRegistry(),
                    Bootstrap_->GetChunkBlockManager(),
                    Bootstrap_->GetMasterClient(),
                    Bootstrap_->GetMasterConnector()->GetLocalDescriptor());
                store->Initialize(descriptor);
                return store;
            }

            case EStoreType::OrderedDynamic:
                return New<TOrderedDynamicStore>(
                    Config_,
                    storeId,
                    tablet);

            default:
                Y_UNREACHABLE();
        }
    }


    void AddTableReplica(TTablet* tablet, const TTableReplicaDescriptor& descriptor)
    {
        auto replicaId = FromProto<TTableReplicaId>(descriptor.replica_id());
        auto& replicas = tablet->Replicas();
        if (replicas.find(replicaId) != replicas.end()) {
            LOG_WARNING_UNLESS(IsRecovery(), "Requested to add an already existing table replica (TabletId: %v, ReplicaId: %v)",
                tablet->GetId(),
                replicaId);
            return;
        }

        auto pair = replicas.emplace(replicaId, TTableReplicaInfo(replicaId));
        YCHECK(pair.second);
        auto& replicaInfo = pair.first->second;

        replicaInfo.SetClusterName(descriptor.cluster_name());
        replicaInfo.SetReplicaPath(descriptor.replica_path());
        replicaInfo.SetStartReplicationTimestamp(descriptor.start_replication_timestamp());
        replicaInfo.SetState(ETableReplicaState::Disabled);
        replicaInfo.MergeFromStatistics(descriptor.statistics());

        if (IsLeader()) {
            StartTableReplicaEpoch(tablet, &replicaInfo);
        }

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Table replica added (TabletId: %v, ReplicaId: %v, ClusterName: %v, ReplicaPath: %v, "
            "StartReplicationTimestamp: %v, CurrentReplicationRowIndex: %v, CurrentReplicationTimestamp: %x)",
            tablet->GetId(),
            replicaId,
            replicaInfo.GetClusterName(),
            replicaInfo.GetReplicaPath(),
            replicaInfo.GetStartReplicationTimestamp(),
            replicaInfo.GetCurrentReplicationRowIndex(),
            replicaInfo.GetCurrentReplicationTimestamp());
    }

    void RemoveTableReplica(TTablet* tablet, const TTableReplicaId& replicaId)
    {
        auto& replicas = tablet->Replicas();
        auto it = replicas.find(replicaId);
        if (it == replicas.end()) {
            LOG_WARNING_UNLESS(IsRecovery(), "Requested to remove a non-existing table replica (TabletId: %v, ReplicaId: %v)",
                tablet->GetId(),
                replicaId);
            return;
        }

        auto& replicaInfo = it->second;

        if (IsLeader()) {
            StopTableReplicaEpoch(&replicaInfo);
        }

        replicas.erase(it);

        UpdateTabletSnapshot(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Table replica removed (TabletId: %v, ReplicaId: %v)",
            tablet->GetId(),
            replicaId);
    }


    void EnableTableReplica(TTablet* tablet, TTableReplicaInfo* replicaInfo)
    {
        LOG_INFO_UNLESS(IsRecovery(), "Table replica state enabled (TabletId: %v, ReplicaId: %v)",
            tablet->GetId(),
            replicaInfo->GetId());

        replicaInfo->SetState(ETableReplicaState::Enabled);

        if (IsLeader()) {
            replicaInfo->GetReplicator()->Enable();
        }
    }

    void DisableTableReplica(TTablet* tablet, TTableReplicaInfo* replicaInfo)
    {
        LOG_INFO_UNLESS(IsRecovery(), "Table replica disabled (TabletId: %v, ReplicaId, "
            "CurrentReplicationRowIndex: %v, CurrentReplicationTimestamp: %v)",
            tablet->GetId(),
            replicaInfo->GetId(),
            replicaInfo->GetCurrentReplicationRowIndex(),
            replicaInfo->GetCurrentReplicationTimestamp());

        replicaInfo->SetState(ETableReplicaState::Disabled);

        if (IsLeader()) {
            replicaInfo->GetReplicator()->Disable();
        }

        PostTableReplicaStatistics(tablet, *replicaInfo);

        {
            TRspDisableTableReplica response;
            ToProto(response.mutable_tablet_id(), tablet->GetId());
            ToProto(response.mutable_replica_id(), replicaInfo->GetId());
            response.set_mount_revision(tablet->GetMountRevision());
            PostMasterMutation(response);
        }
    }

    void PostTableReplicaStatistics(TTablet* tablet, const TTableReplicaInfo& replicaInfo)
    {
        TReqUpdateTableReplicaStatistics request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        ToProto(request.mutable_replica_id(), replicaInfo.GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        replicaInfo.PopulateStatistics(request.mutable_statistics());
        PostMasterMutation(request);
    }


    void UpdateTrimmedRowCount(TTablet* tablet, i64 trimmedRowCount)
    {
        auto prevTrimmedRowCount = tablet->GetTrimmedRowCount();
        if (trimmedRowCount <= prevTrimmedRowCount) {
            return;
        }
        tablet->SetTrimmedRowCount(trimmedRowCount);

        const auto& hiveManager = Slot_->GetHiveManager();
        auto* masterMailbox = Slot_->GetMasterMailbox();

        {
            TReqUpdateTabletTrimmedRowCount masterRequest;
            ToProto(masterRequest.mutable_tablet_id(), tablet->GetId());
            masterRequest.set_mount_revision(tablet->GetMountRevision());
            masterRequest.set_trimmed_row_count(trimmedRowCount);
            hiveManager->PostMessage(masterMailbox, masterRequest);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows trimmed (TabletId: %v, TrimmedRowCount: %v->%v)",
            tablet->GetId(),
            prevTrimmedRowCount,
            trimmedRowCount);
    }

    void AdvanceReplicatedTrimmedRowCount(TTransaction* transaction, TTablet* tablet)
    {
        YCHECK(tablet->IsReplicated());

        if (tablet->Replicas().empty()) {
            return;
        }

        auto minReplicationRowIndex = std::numeric_limits<i64>::max();
        for (const auto& pair : tablet->Replicas()) {
            const auto& replicaInfo = pair.second;
            minReplicationRowIndex = replicaInfo.GetCurrentReplicationRowIndex();
        }

        const auto& storeRowIndexMap = tablet->StoreRowIndexMap();
        if (storeRowIndexMap.empty()) {
            return;
        }

        const auto& config = tablet->GetConfig();
        auto retentionDeadline = TimestampToInstant(transaction->GetCommitTimestamp()).first - config->MinReplicationLogTtl;
        auto it = storeRowIndexMap.find(tablet->GetTrimmedRowCount());
        YCHECK(it != storeRowIndexMap.end());
        while (it != storeRowIndexMap.end()) {
            const auto& store = it->second;
            if (store->IsDynamic()) {
                break;
            }
            if (minReplicationRowIndex < store->GetStartingRowIndex() + store->GetRowCount()) {
                break;
            }
            if (TimestampToInstant(store->GetMaxTimestamp()).first > retentionDeadline) {
                break;
            }
            ++it;
        }

        YCHECK(it != storeRowIndexMap.end());
        auto trimmedRowCount = it->second->GetStartingRowIndex();
        YCHECK(tablet->GetTrimmedRowCount() <= trimmedRowCount);
        UpdateTrimmedRowCount(tablet, trimmedRowCount);
    }


    void OnStoresUpdateCommitSemaphoreAcquired(
        TTablet* tablet,
        const ITransactionPtr& transaction,
        TPromise<void> promise,
        TAsyncSemaphoreGuard /*guard*/)
    {
        try {
            LOG_DEBUG("Started committing tablet stores update transaction (TabletId: %v, TransactionId: %v)",
                tablet->GetId(),
                transaction->GetId());

            WaitFor(transaction->Commit())
                .ThrowOnError();

            LOG_DEBUG("Tablet stores update transaction committed (TabletId: %v, TransactionId: %v)",
                tablet->GetId(),
                transaction->GetId());

            promise.Set();
        } catch (const std::exception& ex) {
            promise.Set(TError(ex));
        }
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTablet, TabletMap_)

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

TTabletManager::~TTabletManager() = default;

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
    TTimestamp transactionStartTimestamp,
    TDuration transactionTimeout,
    TTransactionSignature signature,
    TWireProtocolReader* reader,
    TFuture<void>* commitResult)
{
    return Impl_->Write(
        std::move(tabletSnapshot),
        transactionId,
        transactionStartTimestamp,
        transactionTimeout,
        signature,
        reader,
        commitResult);
}

TFuture<void> TTabletManager::Trim(
    TTabletSnapshotPtr tabletSnapshot,
    i64 trimmedRowCount)
{
    return Impl_->Trim(
        std::move(tabletSnapshot),
        trimmedRowCount);
}

void TTabletManager::ScheduleStoreRotation(TTablet* tablet)
{
    Impl_->ScheduleStoreRotation(tablet);
}

TFuture<void> TTabletManager::CommitTabletStoresUpdateTransaction(
    TTablet* tablet,
    const ITransactionPtr& transaction)
{
    return Impl_->CommitTabletStoresUpdateTransaction(tablet, transaction);
}

IYPathServicePtr TTabletManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

i64 TTabletManager::GetDynamicStoresMemoryUsage() const
{
    return Impl_->GetDynamicStoresMemoryUsage();
}

i64 TTabletManager::GetStaticStoresMemoryUsage() const
{
    return Impl_->GetStaticStoresMemoryUsage();
}

i64 TTabletManager::GetWriteLogsMemoryUsage() const
{
    return Impl_->GetWriteLogsMemoryUsage();
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTablet, *Impl_)

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
