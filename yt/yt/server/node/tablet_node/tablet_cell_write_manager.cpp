#include "tablet_cell_write_manager.h"

#include "automaton.h"
#include "backup_manager.h"
#include "tablet.h"
#include "sorted_store_manager.h"
#include "transaction_manager.h"
#include "transaction.h"
#include "serialize.h"
#include "sorted_dynamic_store.h"
#include "store_manager.h"

#include <yt/yt/server/lib/hydra_common/automaton.h>
#include <yt/yt/server/lib/hydra_common/mutation.h>
#include <yt/yt/server/lib/hydra_common/hydra_manager.h>

#include <yt/yt/server/lib/tablet_node/config.h>
#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/client/transaction_client/helpers.h>

#include <yt/yt/core/compression/codec.h>

#include <library/cpp/yt/small_containers/compact_flat_map.h>

#include <util/generic/cast.h>

namespace NYT::NTabletNode {

using namespace NChaosClient;
using namespace NClusterNode;
using namespace NCompression;
using namespace NHydra;
using namespace NLogging;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletNode::NProto;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellWriteManager
    : public ITabletCellWriteManager
    , public TTabletAutomatonPart
{
    DEFINE_SIGNAL_OVERRIDE(void(TTablet*), ReplicatorWriteTransactionFinished);

public:
    TTabletCellWriteManager(
        ITabletCellWriteManagerHostPtr host,
        ISimpleHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton,
        IInvokerPtr automatonInvoker)
        : TTabletAutomatonPart(
            host->GetCellId(),
            std::move(hydraManager),
            std::move(automaton),
            std::move(automatonInvoker))
        , Host_(std::move(host))
        , ChangelogCodec_(GetCodec(Host_->GetConfig()->ChangelogCodec))
    {
        RegisterMethod(BIND(&TTabletCellWriteManager::HydraFollowerWriteRows, Unretained(this)));
        RegisterMethod(BIND(&TTabletCellWriteManager::HydraWriteDelayedRows, Unretained(this)));
    }

    // ITabletCellWriteManager overrides.

    void Initialize() override
    {
        const auto& transactionManager = Host_->GetTransactionManager();
        transactionManager->SubscribeTransactionPrepared(BIND_NO_PROPAGATE(&TTabletCellWriteManager::OnTransactionPrepared, MakeWeak(this)));
        transactionManager->SubscribeTransactionCommitted(BIND_NO_PROPAGATE(&TTabletCellWriteManager::OnTransactionCommitted, MakeWeak(this)));
        transactionManager->SubscribeTransactionSerialized(BIND_NO_PROPAGATE(&TTabletCellWriteManager::OnTransactionSerialized, MakeWeak(this)));
        transactionManager->SubscribeTransactionAborted(BIND_NO_PROPAGATE(&TTabletCellWriteManager::OnTransactionAborted, MakeWeak(this)));
        transactionManager->SubscribeTransactionTransientReset(BIND_NO_PROPAGATE(&TTabletCellWriteManager::OnTransactionTransientReset, MakeWeak(this)));
    }

    void Write(
        const TTabletSnapshotPtr& tabletSnapshot,
        TTransactionId transactionId,
        TTimestamp transactionStartTimestamp,
        TDuration transactionTimeout,
        TTransactionSignature prepareSignature,
        TTransactionSignature commitSignature,
        TTransactionGeneration generation,
        int rowCount,
        size_t dataWeight,
        bool versioned,
        const TSyncReplicaIdList& syncReplicaIds,
        IWireProtocolReader* reader,
        TFuture<void>* commitResult) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        bool failBeforeExecution = false;
        bool failAfterExecution = false;

        if (auto failureProbability = GetDynamicConfig()->WriteFailureProbability) {
            if (RandomNumber<double>() < *failureProbability) {
                if (RandomNumber<ui32>() % 2 == 0) {
                    failBeforeExecution = true;
                } else {
                    failAfterExecution = true;
                }
            }
        }
        if (failBeforeExecution) {
            THROW_ERROR_EXCEPTION("Test error before write call execution");
        }

        const auto& identity = NRpc::GetCurrentAuthenticationIdentity();
        bool replicatorWrite = IsReplicatorWrite(identity);

        TTablet* tablet = nullptr;
        const auto& transactionManager = Host_->GetTransactionManager();

        auto atomicity = AtomicityFromTransactionId(transactionId);
        if (atomicity == EAtomicity::None) {
            ValidateClientTimestamp(transactionId);
        }

        if (generation > InitialTransactionGeneration) {
            if (versioned) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::WriteRetryIsImpossible,
                    "Retrying versioned writes is not supported");
            }
            if (replicatorWrite) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::WriteRetryIsImpossible,
                    "Retrying replicator writes is not supported");
            }
            if (atomicity == EAtomicity::None) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::WriteRetryIsImpossible,
                    "Retrying non-atomic writes is not supported");
            }
        }

        tabletSnapshot->TabletRuntimeData->ModificationTime = NProfiling::GetInstant();

        auto actualizeTablet = [&] {
            if (!tablet) {
                tablet = Host_->GetTabletOrThrow(tabletSnapshot->TabletId);
                tablet->ValidateMountRevision(tabletSnapshot->MountRevision);
                ValidateTabletMounted(tablet);
            }
        };

        actualizeTablet();

        if (atomicity == EAtomicity::Full) {
            const auto& lockManager = tablet->GetLockManager();
            auto error = lockManager->ValidateTransactionConflict(transactionStartTimestamp);
            if (!error.IsOK()) {
                THROW_ERROR error
                    << TErrorAttribute("tablet_id", tablet->GetId())
                    << TErrorAttribute("transaction_id", transactionId);
            }
        }

        // Due to possible row blocking, serving the request may involve a number of write attempts.
        // Each attempt causes a mutation to be enqueued to Hydra.
        // Since all these mutations are enqueued within a single epoch, only the last commit outcome is
        // actually relevant.
        // Note that we're passing signature to every such call but only the last one actually uses it.
        while (!reader->IsFinished()) {
            // NB: No yielding beyond this point.
            // May access tablet and transaction.

            actualizeTablet();

            ValidateTabletStoreLimit(tablet);

            auto poolTag = Host_->GetDynamicOptions()->EnableTabletDynamicMemoryLimit
                ? tablet->GetPoolTagByMemoryCategory(EMemoryCategory::TabletDynamic)
                : std::nullopt;
            Host_->ValidateMemoryLimit(poolTag);
            ValidateWriteBarrier(replicatorWrite, tablet);

            auto tabletId = tablet->GetId();

            TTransaction* transaction = nullptr;
            bool transactionIsFresh = false;
            bool updateReplicationProgress = false;
            if (atomicity == EAtomicity::Full) {
                transaction = transactionManager->GetOrCreateTransactionOrThrow(
                    transactionId,
                    transactionStartTimestamp,
                    transactionTimeout,
                    true,
                    &transactionIsFresh);
                ValidateTransactionActive(transaction);

                if (generation > transaction->GetTransientGeneration()) {
                    // Promote transaction transient generation and clear the transaction transient state.
                    // In particular, we abort all rows that were prelocked or locked by the previous batches of our generation,
                    // but that is perfectly fine.
                    PromoteTransientGeneration(transaction, generation);
                } else if (generation < transaction->GetTransientGeneration()) {
                    // We may get here in two sitations. The first one is when Write RPC call was late to arrive,
                    // while the second one is trickier. It happens in the case when next generation arrived while our
                    // fiber was waiting on the blocked row. In both cases we are not going to enqueue any more mutations
                    // in order to ensure monotonicity of mutation generations which is an important invariant.
                    YT_LOG_DEBUG(
                        "Stopping obsolete generation write (TabletId: %v, TransactionId: %v, Generation: %x, TransientGeneration: %x)",
                        tabletId,
                        transactionId,
                        generation,
                        transaction->GetTransientGeneration());
                    // Client already decided to go on with the next generation of rows, so we are ok to even ignore
                    // possible commit errors. Note that the result of this particular write does not affect the outcome of the
                    // transaction any more, so we are safe to lose some of freshly enqueued mutations.
                    *commitResult = VoidFuture;
                    return;
                }

                updateReplicationProgress = tablet->GetReplicationCardId() && !versioned;
            } else {
                YT_VERIFY(atomicity == EAtomicity::None);
                if (transactionManager->GetDecommission()) {
                    THROW_ERROR_EXCEPTION("Tablet cell is decommissioned");
                }
            }

            if (transaction) {
                AddTransientAffectedTablet(transaction, tablet);
            }

            auto readerBefore = reader->GetCurrent();

            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            auto context = tabletWriteManager->TransientWriteRows(
                transaction,
                transactionId,
                reader,
                atomicity,
                versioned,
                rowCount,
                dataWeight);

            // For last mutation we use signature from the request,
            // for other mutations signature is zero, see comment above.
            auto mutationPrepareSignature = InitialTransactionSignature;
            auto mutationCommitSignature = InitialTransactionSignature;
            if (reader->IsFinished()) {
                mutationPrepareSignature = prepareSignature;
                mutationCommitSignature = commitSignature;
            }

            auto lockless = context.Lockless;

            YT_LOG_DEBUG_IF(context.RowCount > 0, "Rows written "
                "(TransactionId: %v, TabletId: %v, RowCount: %v, Lockless: %v, "
                "Generation: %x, PrepareSignature: %x, CommitSignature: %x)",
                transactionId,
                tabletId,
                context.RowCount,
                lockless,
                generation,
                mutationPrepareSignature,
                mutationCommitSignature);

            auto readerAfter = reader->GetCurrent();

            if (atomicity == EAtomicity::Full) {
                transaction->TransientPrepareSignature() += mutationPrepareSignature;
            }

            if (readerBefore != readerAfter) {
                auto recordData = reader->Slice(readerBefore, readerAfter);
                auto compressedRecordData = ChangelogCodec_->Compress(recordData);
                TTransactionWriteRecord writeRecord(tabletId, recordData, context.RowCount, context.DataWeight, syncReplicaIds);

                PrelockedTablets_.push(tablet);
                LockTablet(tablet, ETabletLockType::TransientWrite);

                IncrementTabletInFlightMutationCount(tablet, replicatorWrite, +1);

                TReqWriteRows hydraRequest;
                ToProto(hydraRequest.mutable_transaction_id(), transactionId);
                hydraRequest.set_transaction_start_timestamp(transactionStartTimestamp);
                hydraRequest.set_transaction_timeout(ToProto<i64>(transactionTimeout));
                ToProto(hydraRequest.mutable_tablet_id(), tabletId);
                hydraRequest.set_mount_revision(tablet->GetMountRevision());
                hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
                hydraRequest.set_compressed_data(ToString(compressedRecordData));
                hydraRequest.set_prepare_signature(mutationPrepareSignature);
                hydraRequest.set_commit_signature(mutationCommitSignature);
                hydraRequest.set_generation(generation);
                hydraRequest.set_lockless(lockless);
                hydraRequest.set_row_count(writeRecord.RowCount);
                hydraRequest.set_data_weight(writeRecord.DataWeight);
                hydraRequest.set_update_replication_progress(updateReplicationProgress);
                ToProto(hydraRequest.mutable_sync_replica_ids(), syncReplicaIds);
                NRpc::WriteAuthenticationIdentityToProto(&hydraRequest, identity);

                auto mutation = CreateMutation(HydraManager_, hydraRequest);
                mutation->SetHandler(BIND_DONT_CAPTURE_TRACE_CONTEXT(
                    &TTabletCellWriteManager::HydraLeaderWriteRows,
                    MakeStrong(this),
                    transactionId,
                    tablet->GetMountRevision(),
                    mutationPrepareSignature,
                    mutationCommitSignature,
                    generation,
                    lockless,
                    writeRecord,
                    identity,
                    updateReplicationProgress));
                mutation->SetCurrentTraceContext();
                *commitResult = mutation->Commit().As<void>();

                auto counters = tablet->GetTableProfiler()->GetWriteCounters(GetCurrentProfilingUser());
                counters->RowCount.Increment(writeRecord.RowCount);
                counters->DataWeight.Increment(writeRecord.DataWeight);
            } else if (transactionIsFresh) {
                OnTransactionFinished(transaction);
                transactionManager->DropTransaction(transaction);
            }

            // NB: Yielding is now possible.
            // Cannot neither access tablet, nor transaction.
            if (context.BlockedStore) {
                context.BlockedStore->WaitOnBlockedRow(
                    context.BlockedRow,
                    context.BlockedLockMask,
                    context.BlockedTimestamp);
                tablet = nullptr;
            }

            context.Error.ThrowOnError();
        }

        if (failAfterExecution) {
            THROW_ERROR_EXCEPTION("Test error after write call execution");
        }
    }

    // TTabletAutomatonPart overrides.

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::OnStopLeading();

        while (!PrelockedTablets_.empty()) {
            auto* tablet = PrelockedTablets_.front();
            PrelockedTablets_.pop();
            UnlockTablet(tablet, ETabletLockType::TransientWrite);
        }
    }

    void OnAfterSnapshotLoaded() noexcept override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& transactionManager = Host_->GetTransactionManager();
        auto transactions = transactionManager->GetTransactions();

        // COMPAT(gritukan)
        if (transactionManager->GetSnapshotReign() < ETabletReign::ReworkTabletLocks) {
            const auto& transactionManager = Host_->GetTransactionManager();
            // If this fails, you forgot to suspend tablet cells before update.
            YT_VERIFY(transactionManager->IsDecommissioned());
        }

        for (auto* transaction : transactions) {
            YT_VERIFY(GetTransientAffectedTablets(transaction).empty());
            for (auto* tablet : GetPersistentAffectedTablets(transaction)) {
                LockTablet(tablet, ETabletLockType::PersistentTransaction);
            }
        }
    }

private:
    const ITabletCellWriteManagerHostPtr Host_;
    ICodec* const ChangelogCodec_;

    TRingQueue<TTablet*> PrelockedTablets_;

    // NB: Write logs are generally much smaller than dynamic stores,
    // so we don't worry about per-pool management here.
    TMemoryUsageTrackerGuard WriteLogsMemoryTrackerGuard_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    void HydraLeaderWriteRows(
        TTransactionId transactionId,
        NHydra::TRevision mountRevision,
        TTransactionSignature prepareSignature,
        TTransactionSignature commitSignature,
        TTransactionGeneration generation,
        bool lockless,
        const TTransactionWriteRecord& writeRecord,
        const NRpc::TAuthenticationIdentity& identity,
        bool updateReplicationProgress,
        TMutationContext* /*context*/) noexcept
    {
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);
        bool replicatorWrite = IsReplicatorWrite(identity);

        auto atomicity = AtomicityFromTransactionId(transactionId);

        auto* tablet = PrelockedTablets_.front();
        PrelockedTablets_.pop();
        YT_VERIFY(tablet->GetId() == writeRecord.TabletId);
        auto finallyGuard = Finally([&] {
            UnlockTablet(tablet, ETabletLockType::TransientWrite);
        });

        IncrementTabletInFlightMutationCount(tablet, replicatorWrite, -1);

        if (mountRevision != tablet->GetMountRevision()) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Mount revision mismatch; write ignored "
                "(%v, TransactionId: %v, MutationMountRevision: %x, CurrentMountRevision: %x)",
                tablet->GetLoggingTag(),
                transactionId,
                mountRevision,
                tablet->GetMountRevision());
            return;
        }

        TTransaction* transaction = nullptr;
        switch (atomicity) {
            case EAtomicity::Full: {
                const auto& transactionManager = Host_->GetTransactionManager();
                try {
                    // NB: May throw if tablet cell is decommissioned or suspended.
                    transaction = transactionManager->MakeTransactionPersistentOrThrow(transactionId);
                } catch (const std::exception& ex) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), ex, "Failed to make transaction persistent (TabletId: %v, TransactionId: %v)",
                        writeRecord.TabletId,
                        transactionId);
                    return;
                }

                AddPersistentAffectedTablet(transaction, tablet);

                YT_LOG_DEBUG_IF(
                    IsMutationLoggingEnabled(),
                    "Performing atomic write as leader (TabletId: %v, TransactionId: %v, BatchGeneration: %x, "
                    "TransientGeneration: %x, PersistentGeneration: %x)",
                    writeRecord.TabletId,
                    transactionId,
                    generation,
                    transaction->GetTransientGeneration(),
                    transaction->GetPersistentGeneration());

                // Monotonicity of persistent generations is ensured by the early finish in #Write whenever the
                // current batch is obsolete.
                YT_VERIFY(generation >= transaction->GetPersistentGeneration());
                YT_VERIFY(generation <= transaction->GetTransientGeneration());
                if (generation > transaction->GetPersistentGeneration()) {
                    // Promote persistent generation and also clear current persistent transaction state (i.e. write logs).
                    PromotePersistentGeneration(transaction, generation);
                }

                const auto& tabletWriteManager = tablet->GetTabletWriteManager();
                tabletWriteManager->AtomicLeaderWriteRows(transaction, generation, writeRecord, lockless);

                transaction->PersistentPrepareSignature() += prepareSignature;
                // NB: May destroy transaction.
                transactionManager->IncrementCommitSignature(transaction, commitSignature);

                if (updateReplicationProgress) {
                    // Update replication progress for queue replicas so async replicas can pull from them as fast as possible.
                    // NB: This replication progress update is a best effort and does not require tablet locking.
                    transaction->TabletsToUpdateReplicationProgress().insert(tablet->GetId());
                }

                break;
            }

            case EAtomicity::None: {
                const auto& transactionManager = Host_->GetTransactionManager();
                if (transactionManager->GetDecommission()) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet cell is decommissioning, skip non-atomic write");
                    return;
                }

                // This is ensured by a corresponding check in #Write.
                YT_VERIFY(generation == InitialTransactionGeneration);

                if (tablet->GetState() == ETabletState::Orphaned) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet is orphaned; non-atomic write ignored "
                        "(%v, TransactionId: %v)",
                        tablet->GetLoggingTag(),
                        transactionId);
                    return;
                }

                const auto& tabletWriteManager = tablet->GetTabletWriteManager();
                tabletWriteManager->NonAtomicWriteRows(transactionId, writeRecord, /*isLeader*/ true);
                break;
            }

            default:
                YT_ABORT();
        }
    }

    void HydraFollowerWriteRows(TReqWriteRows* request) noexcept
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto atomicity = AtomicityFromTransactionId(transactionId);
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto prepareSignature = request->prepare_signature();
        // COMPAT(gritukan)
        auto commitSignature = request->has_commit_signature() ? request->commit_signature() : prepareSignature;
        auto generation = request->generation();
        auto lockless = request->lockless();
        auto rowCount = request->row_count();
        auto dataWeight = request->data_weight();
        auto syncReplicaIds = FromProto<TSyncReplicaIdList>(request->sync_replica_ids());
        auto updateReplicationProgress = request->update_replication_progress();

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = Host_->FindTablet(tabletId);
        if (!tablet) {
            // NB: Tablet could be missing if it was, e.g., forcefully removed.
            return;
        }

        auto mountRevision = request->mount_revision();
        if (mountRevision != tablet->GetMountRevision()) {
            // Same as above.
            return;
        }

        auto identity = NRpc::ParseAuthenticationIdentityFromProto(*request);
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);

        auto codecId = FromProto<ECodec>(request->codec());
        auto* codec = GetCodec(codecId);
        auto compressedRecordData = TSharedRef::FromString(request->compressed_data());
        auto recordData = codec->Decompress(compressedRecordData);
        TTransactionWriteRecord writeRecord(tabletId, recordData, rowCount, dataWeight, syncReplicaIds);

        switch (atomicity) {
            case EAtomicity::Full: {
                const auto& transactionManager = Host_->GetTransactionManager();
                TTransaction* transaction;
                try {
                    // NB: May throw if tablet cell is decommissioned.
                    transaction = transactionManager->GetOrCreateTransactionOrThrow(
                        transactionId,
                        transactionStartTimestamp,
                        transactionTimeout,
                        false);
                } catch (const std::exception& ex) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), ex, "Failed to create transaction (TransactionId: %v, TabletId: %v)",
                        transactionId,
                        tabletId);
                    return;
                }

                AddPersistentAffectedTablet(transaction, tablet);

                YT_LOG_DEBUG_IF(
                    IsMutationLoggingEnabled(),
                    "Performing atomic write as follower (TabletId: %v, TransactionId: %v, BatchGeneration: %x, PersistentGeneration: %x)",
                    tabletId,
                    transactionId,
                    generation,
                    transaction->GetPersistentGeneration());

                // This invariant holds during recovery.
                YT_VERIFY(transaction->GetPersistentGeneration() == transaction->GetTransientGeneration());
                // Monotonicity of persistent generations is ensured by the early finish in #Write whenever the
                // current batch is obsolete.
                YT_VERIFY(transaction->GetPersistentGeneration() <= generation);
                if (generation > transaction->GetPersistentGeneration()) {
                    // While in recovery, we are responsible for keeping both transient and persistent state up-to-date.
                    // Hence, generation promotion must be handles as a combination of transient and persistent generation promotions
                    // from the regular leader case.
                    PromoteTransientGeneration(transaction, generation);
                    PromotePersistentGeneration(transaction, generation);
                }

                const auto& tabletWriteManager = tablet->GetTabletWriteManager();
                tabletWriteManager->AtomicFollowerWriteRows(transaction, writeRecord, lockless);

                if (updateReplicationProgress) {
                    // Update replication progress for queue replicas so async replicas can pull from them as fast as possible.
                    // NB: This replication progress update is a best effort and does not require tablet locking.
                    transaction->TabletsToUpdateReplicationProgress().insert(tablet->GetId());
                }

                transaction->PersistentPrepareSignature() += prepareSignature;
                transactionManager->IncrementCommitSignature(transaction, commitSignature);

                break;
            }


            case EAtomicity::None: {
                const auto& transactionManager = Host_->GetTransactionManager();
                if (transactionManager->GetDecommission()) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet cell is decommissioning, skip non-atomic write");
                    return;
                }

                // This is ensured by a corresponding check in #Write.
                YT_VERIFY(generation == InitialTransactionGeneration);

                const auto& tabletWriteManager = tablet->GetTabletWriteManager();
                tabletWriteManager->NonAtomicWriteRows(transactionId, writeRecord, /*isLeader*/ false);
                break;
            }

            default:
                YT_ABORT();
        }
    }

    void HydraWriteDelayedRows(TReqWriteDelayedRows* request) noexcept
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        YT_VERIFY(AtomicityFromTransactionId(transactionId) == EAtomicity::Full);

        auto rowCount = request->row_count();
        auto dataWeight = request->data_weight();
        auto commitSignature = request->commit_signature();

        auto* tablet = Host_->FindTablet(tabletId);
        if (!tablet) {
            // NB: Tablet could be missing if it was, e.g., forcefully removed.
            YT_LOG_DEBUG_IF(
                IsMutationLoggingEnabled(),
                "Received delayed rows for unexistent tablet; ignored ",
                "(TabletId: %v, TransactionId: %v)",
                tabletId,
                transactionId);
            return;
        }

        auto mountRevision = FromProto<NHydra::TRevision>(request->mount_revision());
        if (tablet->GetMountRevision() != mountRevision) {
            YT_LOG_DEBUG_IF(
                IsMutationLoggingEnabled(),
                "Received delayed rows with invalid mount revision; ignored "
                "(TabletId: %v, TransactionId: %v, TabletMountRevision: %x, RequestMountRevision: %x)",
                tabletId,
                transactionId,
                tablet->GetMountRevision(),
                mountRevision);
            return;
        }

        auto lockless = request->lockless();

        auto identity = NRpc::ParseAuthenticationIdentityFromProto(*request);
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);

        auto codecId = FromProto<ECodec>(request->codec());
        auto* codec = GetCodec(codecId);
        auto compressedRecordData = TSharedRef::FromString(request->compressed_data());
        auto recordData = codec->Decompress(compressedRecordData);
        TTransactionWriteRecord writeRecord(tabletId, recordData, rowCount, dataWeight, /*syncReplicaIds*/ {});

        const auto& transactionManager = Host_->GetTransactionManager();
        auto* transaction = transactionManager->FindPersistentTransaction(transactionId);

        if (!transaction) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                "Delayed rows sent for absent transaction, ignored "
                "(TransactionId: %v, TabletId: %v, RowCount: %v, DataWeight: %v, CommitSignature: %x)",
                transactionId,
                tablet->GetId(),
                rowCount,
                dataWeight,
                commitSignature);
            return;
        }

        YT_LOG_DEBUG_IF(
            IsMutationLoggingEnabled(),
            "Writing transaction delayed rows (TabletId: %v, TransactionId: %v, RowCount: %v, Lockless: %v, CommitSignature: %x)",
            tablet->GetId(),
            transaction->GetId(),
            writeRecord.RowCount,
            lockless,
            commitSignature);

        auto tabletWriteManager = tablet->GetTabletWriteManager();
        tabletWriteManager->WriteDelayedRows(transaction, writeRecord, lockless);

        // NB: May destroy transaction.
        transactionManager->IncrementCommitSignature(transaction, commitSignature);
    }

    void OnTransactionPrepared(TTransaction* transaction, bool persistent)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext() == persistent);

        auto tablets = persistent
            ? GetPersistentAffectedTablets(transaction)
            : GetTransientAffectedTablets(transaction);

        for (auto* tablet : tablets) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransactionPrepared(transaction, persistent);
        }
    }

    void OnTransactionCommitted(TTransaction* transaction) noexcept
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        for (auto* tablet : GetPersistentAffectedTablets(transaction)) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransactionCommitted(transaction);
        }

        if (!transaction->IsSerializationNeeded()) {
            OnTransactionFinished(transaction);
        }
    }

    void OnTransactionSerialized(TTransaction* transaction) noexcept
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto serializingTabletIds = transaction->SerializingTabletIds();
        for (auto tabletId : serializingTabletIds) {
            auto* tablet = Host_->FindTablet(tabletId);
            if (!tablet) {
                EraseOrCrash(transaction->SerializingTabletIds(), tabletId);
                continue;
            }

            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransactionSerialized(transaction);
        }

        YT_VERIFY(transaction->SerializingTabletIds().empty());

        for (auto tabletId : transaction->TabletsToUpdateReplicationProgress()) {
            auto* tablet = Host_->FindTablet(tabletId);
            if (!tablet) {
                continue;
            }

            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->UpdateReplicationProgress(transaction);
        }

        OnTransactionFinished(transaction);
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        for (auto* tablet : GetAffectedTablets(transaction)) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransactionAborted(transaction);
        }

        OnTransactionFinished(transaction);
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        UnlockLockedTablets(transaction);
    }

    //! This method promotes transaction transient generation and also resets its transient state.
    //! In particular, it aborts all row locks in sorted dynamic stores induced by the transaction,
    //! and resets (transient) lists of prelocked and locked row refs.
    void PromoteTransientGeneration(TTransaction* transaction, TTransactionGeneration generation)
    {
        // This method may be called either with or without a mutation context.

        YT_LOG_DEBUG(
            "Promoting transaction transient generation (TransactionId: %v, TransientGeneration: %x -> %x)",
            transaction->GetId(),
            transaction->GetTransientGeneration(),
            generation);

        transaction->SetTransientGeneration(generation);
        transaction->TransientPrepareSignature() = InitialTransactionSignature;

        for (auto* tablet : GetAffectedTablets(transaction)) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransientGenerationPromoted(transaction);
        }

        // NB: it is ok not to unlock prelocked tablets since tablet locking is a lifetime ensurance mechanism
        // in contrast to row prelocking/locking which is a conflict prevention mechanism. Moreover, we do not
        // want the tablet to become fully unlocked while we still have in flight mutations, so it is better not
        // to touch tablet locks here at all.
    }

    //! This method promotes transaction persistent generation and also resets its persistent state by
    //! clearing all associated write logs.
    void PromotePersistentGeneration(TTransaction* transaction, TTransactionGeneration generation)
    {
        YT_VERIFY(HasMutationContext());

        YT_LOG_DEBUG_IF(
            IsMutationLoggingEnabled(),
            "Promoting transaction persistent generation (TransactionId: %v, PersistentGeneration: %x -> %x)",
            transaction->GetId(),
            transaction->GetPersistentGeneration(),
            generation);

        transaction->SetPersistentGeneration(generation);
        transaction->PersistentPrepareSignature() = InitialTransactionSignature;
        transaction->CommitSignature() = InitialTransactionSignature;

        for (auto* tablet : GetPersistentAffectedTablets(transaction)) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnPersistentGenerationPromoted(transaction);
        }
    }

    void OnTransactionTransientReset(TTransaction* transaction)
    {
        for (auto* tablet : GetAffectedTablets(transaction)) {
            const auto& tabletWriteManager = tablet->GetTabletWriteManager();
            tabletWriteManager->OnTransactionTransientReset(transaction);
        }

        // Releases transient locks.
        UnlockLockedTablets(transaction);
    }

    void ValidateClientTimestamp(TTransactionId transactionId)
    {
        auto clientTimestamp = TimestampFromTransactionId(transactionId);
        auto serverTimestamp = Host_->GetLatestTimestamp();
        auto clientInstant = TimestampToInstant(clientTimestamp).first;
        auto serverInstant = TimestampToInstant(serverTimestamp).first;
        auto clientTimestampThreshold = Host_->GetConfig()->ClientTimestampThreshold;
        if (clientInstant > serverInstant + clientTimestampThreshold ||
            clientInstant < serverInstant - clientTimestampThreshold)
        {
            THROW_ERROR_EXCEPTION("Transaction timestamp is off limits, check the local clock readings")
            << TErrorAttribute("client_timestamp", clientTimestamp)
            << TErrorAttribute("server_timestamp", serverTimestamp);
        }
    }

    void ValidateTabletStoreLimit(TTablet* tablet)
    {
        const auto& mountConfig = tablet->GetSettings().MountConfig;
        auto storeCount = std::ssize(tablet->StoreIdMap());
        auto storeLimit = mountConfig->MaxStoresPerTablet;
        if (storeCount >= storeLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_path", tablet->GetTablePath())
                << TErrorAttribute("store_count", storeCount)
                << TErrorAttribute("store_limit", storeLimit);
        }

        auto overlappingStoreCount = tablet->GetOverlappingStoreCount();
        auto overlappingStoreLimit = mountConfig->MaxOverlappingStoreCount;
        if (overlappingStoreCount >= overlappingStoreLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many overlapping stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_path", tablet->GetTablePath())
                << TErrorAttribute("overlapping_store_count", overlappingStoreCount)
                << TErrorAttribute("overlapping_store_limit", overlappingStoreLimit);
        }

        auto edenStoreCount = tablet->GetEdenStoreCount();
        auto edenStoreCountLimit = mountConfig->MaxEdenStoresPerTablet;
        if (edenStoreCount >= edenStoreCountLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many eden stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_path", tablet->GetTablePath())
                << TErrorAttribute("eden_store_count", edenStoreCount)
                << TErrorAttribute("eden_store_limit", edenStoreCountLimit);
        }

        auto dynamicStoreCount = tablet->GetDynamicStoreCount();
        if (dynamicStoreCount >= DynamicStoreCountLimit) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Too many dynamic stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_path", tablet->GetTablePath())
                << TErrorAttribute("dynamic_store_count", dynamicStoreCount)
                << TErrorAttribute("dynamic_store_count_limit", DynamicStoreCountLimit);
        }

        auto overflow = tablet->GetStoreManager()->CheckOverflow();
        if (!overflow.IsOK()) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::AllWritesDisabled,
                "Active store is overflown, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_path", tablet->GetTablePath())
                << overflow;
        }
    }

    static bool IsReplicatorWrite(const NRpc::TAuthenticationIdentity& identity)
    {
        return identity.User == NSecurityClient::ReplicatorUserName;
    }

    static bool IsReplicatorWrite(TTransaction* transaction)
    {
        return IsReplicatorWrite(transaction->AuthenticationIdentity());
    }

    static void IncrementTabletInFlightMutationCount(TTablet* tablet, bool replicatorWrite, int delta)
    {
        if (replicatorWrite) {
            tablet->SetInFlightReplicatorMutationCount(tablet->GetInFlightReplicatorMutationCount() + delta);
        } else {
            tablet->SetInFlightUserMutationCount(tablet->GetInFlightUserMutationCount() + delta);
        }
    }

    static void ValidateWriteBarrier(bool replicatorWrite, TTablet* tablet)
    {
        if (replicatorWrite) {
            if (tablet->GetInFlightUserMutationCount() > 0) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::ReplicatorWriteBlockedByUser,
                    "Tablet cannot accept replicator writes since some user mutations are still in flight")
                    << TErrorAttribute("tablet_id", tablet->GetId())
                    << TErrorAttribute("table_path", tablet->GetTablePath())
                    << TErrorAttribute("in_flight_mutation_count", tablet->GetInFlightUserMutationCount());
            }
            if (tablet->GetPendingUserWriteRecordCount() > 0) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::ReplicatorWriteBlockedByUser,
                    "Tablet cannot accept replicator writes since some user writes are still pending")
                    << TErrorAttribute("tablet_id", tablet->GetId())
                    << TErrorAttribute("table_path", tablet->GetTablePath())
                    << TErrorAttribute("pending_write_record_count", tablet->GetPendingUserWriteRecordCount());
            }
        } else {
            if (tablet->GetInFlightReplicatorMutationCount() > 0) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::UserWriteBlockedByReplicator,
                    "Tablet cannot accept user writes since some replicator mutations are still in flight")
                    << TErrorAttribute("tablet_id", tablet->GetId())
                    << TErrorAttribute("table_path", tablet->GetTablePath())
                    << TErrorAttribute("in_flight_mutation_count", tablet->GetInFlightReplicatorMutationCount());
            }
            if (tablet->GetPendingReplicatorWriteRecordCount() > 0) {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::UserWriteBlockedByReplicator,
                    "Tablet cannot accept user writes since some replicator writes are still pending")
                    << TErrorAttribute("tablet_id", tablet->GetId())
                    << TErrorAttribute("table_path", tablet->GetTablePath())
                    << TErrorAttribute("pending_write_record_count", tablet->GetPendingReplicatorWriteRecordCount());
            }
        }
    }

    std::vector<TTablet*> GetTabletByIds(const THashSet<TTabletId>& tabletIds)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        std::vector<TTablet*> tablets;
        tablets.reserve(tabletIds.size());
        for (auto tabletId : tabletIds) {
            if (auto* tablet = Host_->FindTablet(tabletId)) {
                tablets.push_back(tablet);
            }
        }

        return tablets;
    }

    void AddTransientAffectedTablet(TTransaction* transaction, TTablet* tablet) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto tabletId = tablet->GetId();
        if (transaction->TransientAffectedTabletIds().emplace(tabletId).second) {
            auto lockCount = LockTablet(tablet, ETabletLockType::TransientTransaction);
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Transaction transiently affects tablet (TransactionId: %v, TabletId: %v, LockCount: %v)",
                transaction->GetId(),
                tablet->GetId(),
                lockCount);
        }
    }

    void AddPersistentAffectedTablet(TTransaction* transaction, TTablet* tablet) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());
        YT_VERIFY(!transaction->GetTransient());

        auto tabletId = tablet->GetId();
        if (transaction->PersistentAffectedTabletIds().emplace(tabletId).second) {
            auto lockCount = LockTablet(tablet, ETabletLockType::PersistentTransaction);
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Transaction persistently affects tablet (TransactionId: %v, TabletId: %v, LockCount: %v)",
                transaction->GetId(),
                tablet->GetId(),
                lockCount);
        }
    }

    std::vector<TTablet*> GetTransientAffectedTablets(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return GetTabletByIds(transaction->TransientAffectedTabletIds());
    }

    std::vector<TTablet*> GetPersistentAffectedTablets(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return GetTabletByIds(transaction->PersistentAffectedTabletIds());
    }

    std::vector<TTablet*> GetAffectedTablets(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return GetTabletByIds(transaction->GetAffectedTabletIds());
    }

    void ValidateTransactionActive(TTransaction* transaction)
    {
        if (transaction->GetTransientState() != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }
    }

    i64 LockTablet(TTablet* tablet, ETabletLockType lockType)
    {
        return Host_->LockTablet(tablet, lockType);
    }

    i64 UnlockTablet(TTablet* tablet, ETabletLockType lockType)
    {
        return Host_->UnlockTablet(tablet, lockType);
    }

    void UnlockLockedTablets(TTransaction* transaction)
    {
        // NB: Transaction may hold both transient and persistent lock on tablet,
        // so #GetAffectedTablets cannot be used here.
        for (auto* tablet : GetTransientAffectedTablets(transaction)) {
            UnlockTablet(tablet, ETabletLockType::TransientTransaction);
        }
        transaction->TransientAffectedTabletIds().clear();

        for (auto* tablet : GetPersistentAffectedTablets(transaction)) {
            UnlockTablet(tablet, ETabletLockType::PersistentTransaction);
        }
        transaction->PersistentAffectedTabletIds().clear();
    }

    TTabletCellWriteManagerDynamicConfigPtr GetDynamicConfig() const
    {
        return Host_->GetDynamicConfig()->TabletCellWriteManager;
    }
};

////////////////////////////////////////////////////////////////////////////////

ITabletCellWriteManagerPtr CreateTabletCellWriteManager(
    ITabletCellWriteManagerHostPtr host,
    ISimpleHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton,
    IInvokerPtr automatonInvoker)
{
    return New<TTabletCellWriteManager>(
        std::move(host),
        std::move(hydraManager),
        std::move(automaton),
        std::move(automatonInvoker));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
