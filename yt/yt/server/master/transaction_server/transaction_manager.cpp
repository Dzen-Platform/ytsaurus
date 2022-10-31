#include "transaction_manager.h"
#include "private.h"
#include "config.h"
#include "boomerang_tracker.h"
#include "transaction_presence_cache.h"
#include "transaction_replication_session.h"
#include "transaction.h"
#include "transaction_proxy.h"

#include <yt/yt/server/master/cell_master/automaton.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/multicell_manager.h>
#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/cypress_server/cypress_manager.h>
#include <yt/yt/server/master/cypress_server/node.h>

#include <yt/yt/server/master/security_server/access_log.h>

#include <yt/yt/server/lib/hive/hive_manager.h>
#include <yt/yt/server/lib/hive/mailbox.h>

#include <yt/yt/server/lib/hydra_common/composite_automaton.h>
#include <yt/yt/server/lib/hydra_common/mutation.h>

#include <yt/yt/server/lib/transaction_server/helpers.h>
#include <yt/yt/server/lib/transaction_server/private.h>

#include <yt/yt/server/lib/transaction_supervisor/transaction_supervisor.h>
#include <yt/yt/server/lib/transaction_supervisor/transaction_lease_tracker.h>
#include <yt/yt/server/lib/transaction_supervisor/transaction_manager_detail.h>

#include <yt/yt/server/master/object_server/attribute_set.h>
#include <yt/yt/server/master/object_server/object.h>
#include <yt/yt/server/master/object_server/type_handler_detail.h>

#include <yt/yt/server/master/security_server/account.h>
#include <yt/yt/server/master/security_server/security_manager.h>
#include <yt/yt/server/master/security_server/user.h>

#include <yt/yt/server/master/sequoia_server/context.h>

#include <yt/yt/server/master/transaction_server/proto/transaction_manager.pb.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/wire_protocol.h>

#include <yt/yt/ytlib/sequoia_client/tables.h>

#include <yt/yt/ytlib/transaction_client/proto/transaction_service.pb.h>
#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/id_generator.h>
#include <yt/yt/core/misc/string.h>

#include <yt/yt/core/profiling/profile_manager.h>

#include <yt/yt/core/ytree/attributes.h>
#include <yt/yt/core/ytree/ephemeral_node_factory.h>
#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/rpc/authentication_identity.h>

namespace NYT::NTransactionServer {

using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NObjectServer;
using namespace NCypressServer;
using namespace NElection;
using namespace NHydra;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;
using namespace NCypressServer;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NTransactionClient::NProto;
using namespace NSecurityServer;
using namespace NProfiling;
using namespace NSequoiaClient;
using namespace NSequoiaServer;
using namespace NTransactionSupervisor;

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TTransactionTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTransaction>
{
public:
    TTransactionTypeHandler(
        TImpl* owner,
        EObjectType objectType);

    ETypeFlags GetFlags() const override
    {
        return ETypeFlags::None;
    }

    EObjectType GetType() const override
    {
        return ObjectType_;
    }

private:
    const EObjectType ObjectType_;


    TCellTagList DoGetReplicationCellTags(const TTransaction* transaction) override
    {
        return transaction->ReplicatedToCellTags();
    }

    IObjectProxyPtr DoGetProxy(TTransaction* transaction, TTransaction* /*dummyTransaction*/) override
    {
        return CreateTransactionProxy(Bootstrap_, &Metadata_, transaction);
    }

    TAccessControlDescriptor* DoFindAcd(TTransaction* transaction) override
    {
        return &transaction->Acd();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TImpl
    : public TMasterAutomatonPart
    , public TTransactionManagerBase<TTransaction>
{
public:
    //! Raised when a new transaction is started.
    DEFINE_SIGNAL(void(TTransaction*), TransactionStarted);

    //! Raised when a transaction is committed.
    DEFINE_SIGNAL(void(TTransaction*), TransactionCommitted);

    //! Raised when a transaction is aborted.
    DEFINE_SIGNAL(void(TTransaction*), TransactionAborted);

    DEFINE_BYREF_RO_PROPERTY(THashSet<TTransaction*>, NativeTopmostTransactions);
    DEFINE_BYREF_RO_PROPERTY(THashSet<TTransaction*>, NativeTransactions);

    DEFINE_BYREF_RO_PROPERTY(TTransactionPresenceCachePtr, TransactionPresenceCache);

    DECLARE_ENTITY_MAP_ACCESSORS(Transaction, TTransaction);

public:
    explicit TImpl(TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, NCellMaster::EAutomatonThreadQueue::TransactionManager)
        , TransactionPresenceCache_(New<TTransactionPresenceCache>(Bootstrap_))
        , BoomerangTracker_(New<TBoomerangTracker>(Bootstrap_))
        , BufferedProducer_(New<TBufferedProducer>())
        , LeaseTracker_(CreateTransactionLeaseTracker(
            Bootstrap_->GetHydraFacade()->GetTransactionTrackerInvoker(),
            TransactionServerLogger))
    {
        TransactionServerProfiler.AddProducer("", BufferedProducer_);

        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Default), AutomatonThread);
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetTransactionTrackerInvoker(), TrackerThread);

        Logger = TransactionServerLogger;

        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraStartTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraStartForeignTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraRegisterTransactionActions, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraPrepareTransactionCommit, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCommitTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraAbortTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraReplicateTransactions, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraNoteNoSuchTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraReturnBoomerang, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraRemoveStuckBoomerangWaves, Unretained(this)));

        RegisterLoader(
            "TransactionManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TransactionManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TransactionManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TransactionManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
    }

    void Initialize()
    {
        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TImpl::OnDynamicConfigChanged, MakeWeak(this)));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::Transaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::NestedTransaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::ExternalizedTransaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::ExternalizedNestedTransaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::UploadTransaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::UploadNestedTransaction));
        objectManager->RegisterHandler(New<TTransactionTypeHandler>(this, EObjectType::AtomicTabletTransaction));

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::Periodic),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            TDynamicTransactionManagerConfig::DefaultProfilingPeriod);
        ProfilingExecutor_->Start();
    }

    const TTransactionPresenceCachePtr& GetTransactionPresenceCache()
    {
        return TransactionPresenceCache_;
    }

    TTransaction* StartTransaction(
        TTransaction* parent,
        std::vector<TTransaction*> prerequisiteTransactions,
        const TCellTagList& replicatedToCellTags,
        std::optional<TDuration> timeout,
        std::optional<TInstant> deadline,
        const std::optional<TString>& title,
        const IAttributeDictionary& attributes,
	TTransactionId hintId = NullTransactionId)
    {
        ValidateNativeTransactionStart(parent, prerequisiteTransactions);

        return DoStartTransaction(
            /*upload*/ false,
            parent,
            std::move(prerequisiteTransactions),
            replicatedToCellTags,
            timeout,
            deadline,
            title,
            attributes,
            hintId);
    }

    TTransaction* StartUploadTransaction(
        TTransaction* parent,
        const TCellTagList& replicatedToCellTags,
        std::optional<TDuration> timeout,
        const std::optional<TString>& title,
        TTransactionId hintId)
    {
        ValidateUploadTransactionStart(hintId, parent);

        return DoStartTransaction(
            true /*upload*/,
            parent,
            {} /*prerequisiteTransactions*/,
            replicatedToCellTags,
            timeout,
            std::nullopt /*deadline*/,
            title,
            EmptyAttributes(),
            hintId);
    }

    void ValidateGenericTransactionStart(TTransaction* parent)
    {
        if (!parent) {
            return;
        }

        if (parent->IsUpload()) {
            THROW_ERROR_EXCEPTION(
                NTransactionClient::EErrorCode::UploadTransactionCannotHaveNested,
                "Failed to start a transaction nested in an upload transaction")
                << TErrorAttribute("upload_transaction_id", parent->GetId());
        }
    }

    void ValidateNativeTransactionStart(
        TTransaction* parent,
        const std::vector<TTransaction*>& prerequisiteTransactions)
    {
        ValidateGenericTransactionStart(parent);

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto thisCellTag = multicellManager->GetCellTag();

        if (parent && CellTagFromId(parent->GetId()) != thisCellTag) {
            THROW_ERROR_EXCEPTION(
                NTransactionClient::EErrorCode::ForeignParentTransaction,
                "Parent transaction is foreign")
                << TErrorAttribute("parent_transaction_id", parent->GetId())
                << TErrorAttribute("parent_transaction_cell_tag", CellTagFromId(parent->GetId()))
                << TErrorAttribute("expected_cell_tag", thisCellTag);
        }

        for (auto* prerequisiteTransaction : prerequisiteTransactions) {
            if (CellTagFromId(prerequisiteTransaction->GetId()) != thisCellTag) {
                THROW_ERROR_EXCEPTION(
                    NTransactionClient::EErrorCode::ForeignPrerequisiteTransaction,
                    "Prerequisite transaction is foreign")
                    << TErrorAttribute("prerequisite_transaction_id", prerequisiteTransaction->GetId())
                    << TErrorAttribute("prerequisite_transaction_cell_tag", CellTagFromId(prerequisiteTransaction->GetId()))
                    << TErrorAttribute("expected_cell_tag", thisCellTag);
            }
        }
    }

    void ValidateUploadTransactionStart(TTransactionId hintId, TTransaction* parent)
    {
        if (hintId &&
            TypeFromId(hintId) != EObjectType::UploadTransaction &&
            TypeFromId(hintId) != EObjectType::UploadNestedTransaction &&
            GetDynamicConfig()->EnableDedicatedUploadTransactionObjectTypes)
        {
            if (IsHiveMutation()) {
                // COMPAT(shakurov)
                // This is a hive mutation posted by a pre-20.3 master (and being
                // applied by a post-20.3 one).
                YT_LOG_ALERT_UNLESS(IsRecovery(),
                    "Upload transaction has generic type despite dedicated types being enabled (TransactionId: %v)",
                    hintId);
            } else {
                YT_ABORT();
            }
        }

        ValidateGenericTransactionStart(parent);
    }

    TTransaction* DoStartTransaction(
        bool upload,
        TTransaction* parent,
        std::vector<TTransaction*> prerequisiteTransactions,
        TCellTagList replicatedToCellTags,
        std::optional<TDuration> timeout,
        std::optional<TInstant> deadline,
        const std::optional<TString>& title,
        const IAttributeDictionary& attributes,
        TTransactionId hintId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NProfiling::TWallTimer timer;

        const auto& dynamicConfig = GetDynamicConfig();

        auto transactionObjectType = upload && dynamicConfig->EnableDedicatedUploadTransactionObjectTypes
            ? (parent ? EObjectType::UploadNestedTransaction : EObjectType::UploadTransaction)
            : (parent ? EObjectType::NestedTransaction : EObjectType::Transaction);

        if (parent) {
            if (parent->GetPersistentState() != ETransactionState::Active) {
                parent->ThrowInvalidState();
            }

            if (parent->GetDepth() >= dynamicConfig->MaxTransactionDepth) {
                THROW_ERROR_EXCEPTION(
                    NTransactionClient::EErrorCode::TransactionDepthLimitReached,
                    "Transaction depth limit reached")
                    << TErrorAttribute("limit", dynamicConfig->MaxTransactionDepth);
            }
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto transactionId = objectManager->GenerateId(transactionObjectType, hintId);

        auto transactionHolder = TPoolAllocator::New<TTransaction>(transactionId, upload);
        auto* transaction = TransactionMap_.Insert(transactionId, std::move(transactionHolder));

        // Every active transaction has a fake reference to itself.
        YT_VERIFY(transaction->RefObject() == 1);

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto native = (CellTagFromId(transactionId) == multicellManager->GetCellTag());

        if (parent) {
            transaction->SetParent(parent);
            transaction->SetDepth(parent->GetDepth() + 1);
            YT_VERIFY(parent->NestedTransactions().insert(transaction).second);
            objectManager->RefObject(transaction);
        }

        if (native) {
            YT_VERIFY(NativeTransactions_.insert(transaction).second);
            if (!parent) {
                YT_VERIFY(NativeTopmostTransactions_.insert(transaction).second);
            }
        }

        transaction->SetPersistentState(ETransactionState::Active);
        transaction->PrerequisiteTransactions() = std::move(prerequisiteTransactions);
        for (auto* prerequisiteTransaction : transaction->PrerequisiteTransactions()) {
            // NB: Duplicates are fine; prerequisite transactions may be duplicated.
            prerequisiteTransaction->DependentTransactions().insert(transaction);
        }


        if (!native) {
            transaction->SetForeign();
        }

        if (native && timeout) {
            transaction->SetTimeout(std::min(*timeout, dynamicConfig->MaxTransactionTimeout));
        }

        if (native) {
            transaction->SetDeadline(deadline);
        }

        if (IsLeader()) {
            CreateLease(transaction);
        }

        transaction->SetTitle(title);

        // NB: This is not quite correct for replicated transactions but we don't care.
        const auto* mutationContext = GetCurrentMutationContext();
        transaction->SetStartTime(mutationContext->GetTimestamp());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto* user = securityManager->GetAuthenticatedUser();
        transaction->Acd().SetOwner(user);

        objectManager->FillAttributes(transaction, attributes);

        if (!replicatedToCellTags.empty()) {
            // Never include native cell tag into ReplicatedToCellTags.
            replicatedToCellTags.erase(
                std::remove(
                    replicatedToCellTags.begin(),
                    replicatedToCellTags.end(),
                    CellTagFromId(transactionId)),
                replicatedToCellTags.end());

            if (upload) {
                transaction->ReplicatedToCellTags() = replicatedToCellTags;
            } else {
                ReplicateTransaction(transaction, replicatedToCellTags);
            }
        }

        TransactionStarted_.Fire(transaction);

        auto time = timer.GetElapsedTime();

        YT_LOG_ACCESS("StartTransaction", transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction started (TransactionId: %v, ParentId: %v, PrerequisiteTransactionIds: %v, "
            "ReplicatedToCellTags: %v, Timeout: %v, Deadline: %v, User: %v, Title: %v, WallTime: %v)",
            transactionId,
            GetObjectId(parent),
            MakeFormattableView(transaction->PrerequisiteTransactions(), [] (auto* builder, const auto* prerequisiteTransaction) {
                FormatValue(builder, prerequisiteTransaction->GetId(), TStringBuf());
            }),
            replicatedToCellTags,
            transaction->GetTimeout(),
            transaction->GetDeadline(),
            user->GetName(),
            title,
            time);

        securityManager->ChargeUser(user, {EUserWorkloadType::Write, 1, time});

        CacheTransactionStarted(transaction);

        return transaction;
    }

    void CommitTransaction(
        TTransaction* transaction,
        const TTransactionCommitOptions& options)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NProfiling::TWallTimer timer;

        YT_VERIFY(transaction->IsForeign() || transaction->GetNativeCommitMutationRevision() == NHydra::NullRevision);

        auto transactionId = transaction->GetId();

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::Committed) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction is already committed (TransactionId: %v)",
                transactionId);
            return;
        }

        if (state != ETransactionState::Active &&
            state != ETransactionState::PersistentCommitPrepared)
        {
            transaction->ThrowInvalidState();
        }

        bool temporaryRefTimestampHolder = false;
        if (!transaction->LockedDynamicTables().empty()) {
            // Usually ref is held by chunk views in branched tables. However, if
            // all tables are empty no natural ref exist, so we have to take it here.
            temporaryRefTimestampHolder = true;
            CreateOrRefTimestampHolder(transactionId);

            SetTimestampHolderTimestamp(transactionId, options.CommitTimestamp);
        }

        TCompactVector<TTransaction*, 16> nestedTransactions(
            transaction->NestedTransactions().begin(),
            transaction->NestedTransactions().end());
        std::sort(nestedTransactions.begin(), nestedTransactions.end(), TObjectIdComparer());
        for (auto* nestedTransaction : nestedTransactions) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Aborting nested transaction on parent commit (TransactionId: %v, ParentId: %v)",
                nestedTransaction->GetId(),
                transactionId);
            TTransactionAbortOptions options{
                .Force = true,
            };
            AbortTransaction(nestedTransaction, options);
        }
        YT_VERIFY(transaction->NestedTransactions().empty());

        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        if (!transaction->ReplicatedToCellTags().empty()) {
            NProto::TReqCommitTransaction request;
            ToProto(request.mutable_transaction_id(), transactionId);
            request.set_commit_timestamp(options.CommitTimestamp);
            const auto* mutationContext = NHydra::GetCurrentMutationContext();
            request.set_native_commit_mutation_revision(mutationContext->GetVersion().ToRevision());
            multicellManager->PostToMasters(request, transaction->ReplicatedToCellTags());
        }

        if (!transaction->ExternalizedToCellTags().empty()) {
            NProto::TReqCommitTransaction request;
            ToProto(request.mutable_transaction_id(), MakeExternalizedTransactionId(transactionId, multicellManager->GetCellTag()));
            request.set_commit_timestamp(options.CommitTimestamp);
            const auto* mutationContext = NHydra::GetCurrentMutationContext();
            request.set_native_commit_mutation_revision(mutationContext->GetVersion().ToRevision());
            multicellManager->PostToMasters(request, transaction->ExternalizedToCellTags());
        }

        if (IsLeader()) {
            CloseLease(transaction);
        }

        transaction->SetPersistentState(ETransactionState::Committed);

        TransactionCommitted_.Fire(transaction);

        if (temporaryRefTimestampHolder) {
            UnrefTimestampHolder(transactionId);
        }

	    auto sequoiaContextGuard = CreateSequoiaContextGuard(transaction);

        RunCommitTransactionActions(transaction, options);

        if (auto* parent = transaction->GetParent()) {
            parent->ExportedObjects().insert(
                parent->ExportedObjects().end(),
                transaction->ExportedObjects().begin(),
                transaction->ExportedObjects().end());
            parent->ImportedObjects().insert(
                parent->ImportedObjects().end(),
                transaction->ImportedObjects().begin(),
                transaction->ImportedObjects().end());

            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->RecomputeTransactionAccountResourceUsage(parent);
        } else {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            for (auto* object : transaction->ImportedObjects()) {
                objectManager->UnrefObject(object);
            }
        }
        transaction->ExportedObjects().clear();
        transaction->ImportedObjects().clear();

        auto* user = transaction->Acd().GetOwner()->AsUser();

        YT_LOG_ACCESS("CommitTransaction", transaction);

        FinishTransaction(transaction);

        auto time = timer.GetElapsedTime();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Transaction committed (TransactionId: %v, User: %v, CommitTimestamp: %v@%v, WallTime: %v)",
            transactionId,
            user->GetName(),
            options.CommitTimestamp,
            options.CommitTimestampClusterTag,
            time);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ChargeUser(user, {EUserWorkloadType::Write, 1, time});
    }

    void AbortTransaction(
        TTransaction* transaction,
        const TTransactionAbortOptions& options,
        bool validatePermissions = true)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NProfiling::TWallTimer timer;

        auto transactionId = transaction->GetId();

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::Aborted) {
            return;
        }

        if (state == ETransactionState::PersistentCommitPrepared && !options.Force ||
            state == ETransactionState::Committed)
        {
            transaction->ThrowInvalidState();
        }

        if (validatePermissions) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(transaction, EPermission::Write);
        }

        TCompactVector<TTransaction*, 16> nestedTransactions(
            transaction->NestedTransactions().begin(),
            transaction->NestedTransactions().end());
        std::sort(nestedTransactions.begin(), nestedTransactions.end(), TObjectIdComparer());
        for (auto* nestedTransaction : nestedTransactions) {
            TTransactionAbortOptions options{
                .Force = true,
            };
            AbortTransaction(nestedTransaction, options, /*validatePermissions*/ false);
        }
        YT_VERIFY(transaction->NestedTransactions().empty());

        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        if (!transaction->ReplicatedToCellTags().empty()) {
            NProto::TReqAbortTransaction request;
            ToProto(request.mutable_transaction_id(), transactionId);
            request.set_force(true);
            multicellManager->PostToMasters(request, transaction->ReplicatedToCellTags());
        }

        if (!transaction->ExternalizedToCellTags().empty()) {
            NProto::TReqAbortTransaction request;
            ToProto(request.mutable_transaction_id(), MakeExternalizedTransactionId(transactionId, multicellManager->GetCellTag()));
            request.set_force(true);
            multicellManager->PostToMasters(request, transaction->ExternalizedToCellTags());
        }

        if (IsLeader()) {
            CloseLease(transaction);
        }

        transaction->SetPersistentState(ETransactionState::Aborted);

        TransactionAborted_.Fire(transaction);

        RunAbortTransactionActions(transaction, options);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        for (const auto& entry : transaction->ExportedObjects()) {
            auto* object = entry.Object;
            objectManager->UnrefObject(object);
            const auto& handler = objectManager->GetHandler(object);
            handler->UnexportObject(object, entry.DestinationCellTag, 1);
        }
        for (auto* object : transaction->ImportedObjects()) {
            objectManager->UnrefObject(object);
            object->ImportUnrefObject();
        }
        transaction->ExportedObjects().clear();
        transaction->ImportedObjects().clear();

        auto* user = transaction->Acd().GetOwner()->AsUser();

        YT_LOG_ACCESS("AbortTransaction", transaction);

        FinishTransaction(transaction);

        auto time = timer.GetElapsedTime();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction aborted (TransactionId: %v, User: %v, Force: %v, WallTime: %v)",
            transactionId,
            user->GetName(),
            options.Force,
            time);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ChargeUser(user, {EUserWorkloadType::Write, 1, time});
    }

    TTransactionId ReplicateTransaction(TTransaction* transaction, TCellTagList dstCellTags)
    {
        YT_VERIFY(IsObjectAlive(transaction));
        YT_VERIFY(transaction->IsNative());
        // NB: native transactions are always replicated, not externalized.
        return ExternalizeTransaction(transaction, dstCellTags);
    }

    TTransactionId ExternalizeTransaction(TTransaction* transaction, TCellTagList dstCellTags)
    {
        if (!transaction) {
            return {};
        }

        if (transaction->IsUpload()) {
            return transaction->GetId();
        }

        auto checkTransactionState = [&] (TTransaction* transactionToCheck) {
            auto state = transactionToCheck->GetPersistentState();
            if (state != ETransactionState::Committed && state != ETransactionState::Aborted) {
                return;
            }

            if (transactionToCheck == transaction) {
                YT_LOG_ALERT_UNLESS(IsRecovery(),
                    "Unexpected transaction state encountered while replicating (TransactionId: %v, TransactionState: %Qlv)",
                    transaction->GetId(),
                    state);
            } else {
                YT_LOG_ALERT_UNLESS(IsRecovery(),
                    "Unexpected ancestor transaction state encountered while replicating (TransactionId: %v, AncestorTransactionId: %v, AncestorTransactionState: %Qlv)",
                    transaction->GetId(),
                    transactionToCheck->GetId(),
                    state);
            }
        };

        // Shall externalize if true, replicate otherwise.
        auto shouldExternalize = transaction->IsForeign();

        TCompactVector<std::pair<TTransaction*, TCellTagList>, 16> transactionsToDstCells;
        for (auto* currentTransaction = transaction; currentTransaction; currentTransaction = currentTransaction->GetParent()) {
            YT_VERIFY(IsObjectAlive(currentTransaction));
            checkTransactionState(currentTransaction);

            transactionsToDstCells.emplace_back(currentTransaction, TCellTagList());

            for (auto dstCellTag : dstCellTags) {
                if (shouldExternalize) {
                    if (currentTransaction->IsExternalizedToCell(dstCellTag)) {
                        continue;
                    }
                    currentTransaction->ExternalizedToCellTags().push_back(dstCellTag);
                } else {
                    if (currentTransaction->IsReplicatedToCell(dstCellTag)) {
                        continue;
                    }
                    currentTransaction->ReplicatedToCellTags().push_back(dstCellTag);
                }

                transactionsToDstCells.back().second.push_back(dstCellTag);
            }

            if (transactionsToDstCells.back().second.empty()) {
                // Already present on all dst cells.
                transactionsToDstCells.pop_back();
                break;
            }
        }

        std::reverse(transactionsToDstCells.begin(), transactionsToDstCells.end());

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        for (const auto& [currentTransaction, cellTags] : transactionsToDstCells) {
            auto transactionId = currentTransaction->GetId();
            auto parentTransactionId = GetObjectId(currentTransaction->GetParent());

            auto effectiveTransactionId = transactionId;
            auto effectiveParentTransactionId = parentTransactionId;

            if (shouldExternalize) {
                effectiveTransactionId = MakeExternalizedTransactionId(transactionId, multicellManager->GetCellTag());
                effectiveParentTransactionId = MakeExternalizedTransactionId(parentTransactionId, multicellManager->GetCellTag());

                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Externalizing transaction (TransactionId: %v, ParentTransactionId: %v, DstCellTags: %v, ExternalizedTransactionId: %v, ExternalizedParentTransactionId: %v)",
                    transactionId,
                    parentTransactionId,
                    cellTags,
                    effectiveTransactionId,
                    effectiveParentTransactionId);
            } else {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replicating transaction (TransactionId: %v, ParentTransactionId: %v, DstCellTags: %v)",
                    transactionId,
                    parentTransactionId,
                    cellTags);
            }

            // NB: technically, an externalized transaction *is* foreign, with its native cell being this one.
            // And it *is* coordinated by this cell, even though there's no corresponding 'native' object.

            NTransactionServer::NProto::TReqStartForeignTransaction startRequest;
            ToProto(startRequest.mutable_id(), effectiveTransactionId);
            if (effectiveParentTransactionId) {
                ToProto(startRequest.mutable_parent_id(), effectiveParentTransactionId);
            }
            if (currentTransaction->GetTitle()) {
                startRequest.set_title(*currentTransaction->GetTitle());
            }
            startRequest.set_upload(currentTransaction->IsUpload());
            if (const auto* attributes = transaction->GetAttributes()) {
                if (auto operationType = attributes->Find("operation_type")) {
                    startRequest.set_operation_type(ConvertTo<TString>(operationType));
                }
                if (auto operationId = attributes->Find("operation_id")) {
                    startRequest.set_operation_id(ConvertTo<TString>(operationId));
                }
                if (auto operationTitle = attributes->Find("operation_title")) {
                    startRequest.set_operation_title(ConvertTo<TString>(operationTitle));
                }
            }
            multicellManager->PostToMasters(startRequest, cellTags);
        }

        return shouldExternalize
            ? MakeExternalizedTransactionId(transaction->GetId(), multicellManager->GetCellTag())
            : transaction->GetId();
    }

    TTransactionId GetNearestExternalizedTransactionAncestor(
        TTransaction* transaction,
        TCellTag dstCellTag)
    {
        if (!transaction) {
            return {};
        }

        if (transaction->IsUpload()) {
            return transaction->GetId();
        }

        // Find nearest externalized transaction if true, replicated transaction if false;
        auto externalized = transaction->IsForeign();

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        for (auto* currentTransaction = transaction; currentTransaction; currentTransaction = currentTransaction->GetParent()) {
            if (externalized && currentTransaction->IsExternalizedToCell(dstCellTag)) {
                return MakeExternalizedTransactionId(currentTransaction->GetId(), multicellManager->GetCellTag());
            }

            if (!externalized && currentTransaction->IsReplicatedToCell(dstCellTag)) {
                return currentTransaction->GetId();
            }
        }

        return {};
    }

    TTransaction* GetTransactionOrThrow(TTransactionId transactionId)
    {
        Bootstrap_->VerifyPersistentStateRead();

        auto* transaction = FindTransaction(transactionId);
        if (!IsObjectAlive(transaction)) {
            ThrowNoSuchTransaction(transactionId);
        }
        return transaction;
    }

    TFuture<TInstant> GetLastPingTime(const TTransaction* transaction)
    {
        Bootstrap_->VerifyPersistentStateRead();

        return LeaseTracker_->GetLastPingTime(transaction->GetId());
    }

    void SetTransactionTimeout(
        TTransaction* transaction,
        TDuration timeout)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        transaction->SetTimeout(timeout);

        if (IsLeader()) {
            LeaseTracker_->SetTimeout(transaction->GetId(), timeout);
        }
    }

    void StageObject(TTransaction* transaction, TObject* object)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_VERIFY(transaction->StagedObjects().insert(object).second);
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(object);
    }

    void UnstageObject(TTransaction* transaction, TObject* object, bool recursive)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& handler = objectManager->GetHandler(object);
        handler->UnstageObject(object, recursive);

        if (transaction) {
            YT_VERIFY(transaction->StagedObjects().erase(object) == 1);
            objectManager->UnrefObject(object);
        }
    }

    void StageNode(TTransaction* transaction, TCypressNode* trunkNode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_ASSERT(trunkNode->IsTrunk());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        transaction->StagedNodes().push_back(trunkNode);
        objectManager->RefObject(trunkNode);
    }

    void ImportObject(TTransaction* transaction, TObject* object)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        transaction->ImportedObjects().push_back(object);
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(object);
        object->ImportRefObject();
    }

    void ExportObject(TTransaction* transaction, TObject* object, TCellTag destinationCellTag)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        transaction->ExportedObjects().push_back({object, destinationCellTag});

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(object);

        const auto& handler = objectManager->GetHandler(object);
        handler->ExportObject(object, destinationCellTag);
    }

    std::unique_ptr<TMutation> CreateStartTransactionMutation(
        TCtxStartTransactionPtr context,
        const NTransactionServer::NProto::TReqStartTransaction& request)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            request,
            &TImpl::HydraStartTransaction,
            this);
    }

    std::unique_ptr<TMutation> CreateRegisterTransactionActionsMutation(TCtxRegisterTransactionActionsPtr context)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            &TImpl::HydraRegisterTransactionActions,
            this);
    }

    std::unique_ptr<TMutation> CreateReplicateTransactionsMutation(TCtxReplicateTransactionsPtr context)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            &TImpl::HydraReplicateTransactions,
            this);
    }

    // ITransactionManager implementation.
    TFuture<void> GetReadyToPrepareTransactionCommit(
        const std::vector<TTransactionId>& prerequisiteTransactionIds,
        const std::vector<TCellId>& cellIdsToSyncWith)
    {
        if (prerequisiteTransactionIds.empty() && cellIdsToSyncWith.empty()) {
            return VoidFuture;
        }

        std::vector<TFuture<void>> asyncResults;
        asyncResults.reserve(cellIdsToSyncWith.size() + 1);

        if (!prerequisiteTransactionIds.empty()) {
            asyncResults.push_back(RunTransactionReplicationSession(false, Bootstrap_, prerequisiteTransactionIds, {}));
        }

        if (!cellIdsToSyncWith.empty()) {
            const auto& hiveManager = Bootstrap_->GetHiveManager();
            for (auto cellId : cellIdsToSyncWith) {
                asyncResults.push_back(hiveManager->SyncWith(cellId, true));
            }
        }

        return AllSucceeded(std::move(asyncResults));
    }

    void PrepareTransactionCommit(
        TTransactionId transactionId,
        const TTransactionPrepareOptions& options)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);
        auto persistent = options.Persistent;

        // Allow preparing transactions in Active and TransientCommitPrepared (for persistent mode) states.
        // This check applies not only to #transaction itself but also to all of its ancestors.
        {
            auto* currentTransaction = transaction;
            while (currentTransaction) {
                auto state = currentTransaction->GetState(persistent);
                if (state != ETransactionState::Active) {
                    currentTransaction->ThrowInvalidState();
                }
                currentTransaction = currentTransaction->GetParent();
            }
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(transaction, EPermission::Write);

        auto state = transaction->GetState(persistent);
        if (state != ETransactionState::Active) {
            return;
        }

        for (auto prerequisiteTransactionId : options.PrerequisiteTransactionIds) {
            ValidatePrerequisiteTransaction(prerequisiteTransactionId);
        }

        auto sequoiaContextGuard = CreateSequoiaContextGuard(transaction);

        RunPrepareTransactionActions(transaction, options);

        if (persistent) {
            transaction->SetPersistentState(ETransactionState::PersistentCommitPrepared);
        } else {
            transaction->SetTransientState(ETransactionState::TransientCommitPrepared);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Transaction commit prepared (TransactionId: %v, Persistent: %v, PrepareTimestamp: %v@%v)",
            transactionId,
            persistent,
            options.PrepareTimestamp,
            options.PrepareTimestampClusterTag);
    }

    void PrepareTransactionAbort(TTransactionId transactionId, const TTransactionAbortOptions& options)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);
        auto force = options.Force;

        auto state = transaction->GetTransientState();
        if (state != ETransactionState::Active && !force) {
            transaction->ThrowInvalidState();
        }

        if (state != ETransactionState::Active) {
            return;
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);
        securityManager->ValidatePermission(transaction, EPermission::Write);

        transaction->SetTransientState(ETransactionState::TransientAbortPrepared);

        YT_LOG_DEBUG("Transaction abort prepared (TransactionId: %v)",
            transactionId);
    }

    void CommitTransaction(
        TTransactionId transactionId,
        const TTransactionCommitOptions& options,
        NHydra::TRevision nativeCommitMutationRevision)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);
        transaction->SetNativeCommitMutationRevision(nativeCommitMutationRevision);
        CommitTransaction(transaction, options);
    }

    void AbortTransaction(
        TTransactionId transactionId,
        const TTransactionAbortOptions& options)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);
        AbortTransaction(transaction, options);
    }

    void PingTransaction(
        TTransactionId transactionId,
        bool pingAncestors)
    {
        VERIFY_THREAD_AFFINITY(TrackerThread);

        LeaseTracker_->PingTransaction(transactionId, pingAncestors);
    }

    void CreateOrRefTimestampHolder(TTransactionId transactionId)
    {
        if (auto it = TimestampHolderMap_.find(transactionId)) {
            ++it->second.RefCount;
        }
        TimestampHolderMap_.emplace(transactionId, TTimestampHolder{});
    }

    void SetTimestampHolderTimestamp(TTransactionId transactionId, TTimestamp timestamp)
    {
        if (auto it = TimestampHolderMap_.find(transactionId)) {
            it->second.Timestamp = timestamp;
        }
    }

    TTimestamp GetTimestampHolderTimestamp(TTransactionId transactionId)
    {
        if (auto it = TimestampHolderMap_.find(transactionId)) {
            return it->second.Timestamp;
        }
        return NullTimestamp;
    }

    void UnrefTimestampHolder(TTransactionId transactionId)
    {
        if (auto it = TimestampHolderMap_.find(transactionId)) {
            --it->second.RefCount;
            if (it->second.RefCount == 0) {
                TimestampHolderMap_.erase(it);
            }
        }
    }

private:
    struct TTimestampHolder
    {
        TTimestamp Timestamp = NullTimestamp;
        i64 RefCount = 1;

        void Persist(const NCellMaster::TPersistenceContext& context)
        {
            using ::NYT::Persist;
            Persist(context, Timestamp);
            Persist(context, RefCount);
        }
    };

    friend class TTransactionTypeHandler;

    const TBoomerangTrackerPtr BoomerangTracker_;

    NProfiling::TBufferedProducerPtr BufferedProducer_;
    NConcurrency::TPeriodicExecutorPtr ProfilingExecutor_;

    const ITransactionLeaseTrackerPtr LeaseTracker_;

    NHydra::TEntityMap<TTransaction> TransactionMap_;

    THashMap<TTransactionId, TTimestampHolder> TimestampHolderMap_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
    DECLARE_THREAD_AFFINITY_SLOT(TrackerThread);


    void HydraStartTransaction(
        const TCtxStartTransactionPtr& context,
        NTransactionServer::NProto::TReqStartTransaction* request,
        NTransactionServer::NProto::TRspStartTransaction* response)
    {
        auto identity = NRpc::ParseAuthenticationIdentityFromProto(*request);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, std::move(identity));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto* schema = objectManager->GetSchema(EObjectType::Transaction);
        securityManager->ValidatePermission(schema, EPermission::Create);

        auto parentId = FromProto<TTransactionId>(request->parent_id());
        auto* parent = parentId ? GetTransactionOrThrow(parentId) : nullptr;

        auto prerequisiteTransactionIds = FromProto<std::vector<TTransactionId>>(request->prerequisite_transaction_ids());
        std::vector<TTransaction*> prerequisiteTransactions;
        for (auto id : prerequisiteTransactionIds) {
            auto* prerequisiteTransaction = ValidatePrerequisiteTransaction(id);
            prerequisiteTransactions.push_back(prerequisiteTransaction);
        }

        auto attributes = request->has_attributes()
            ? FromProto(request->attributes())
            : CreateEphemeralAttributes();

        auto title = request->has_title() ? std::make_optional(request->title()) : std::nullopt;

        auto timeout = FromProto<TDuration>(request->timeout());

        std::optional<TInstant> deadline;
        if (request->has_deadline()) {
            deadline = FromProto<TInstant>(request->deadline());
        }

        TCellTagList replicateToCellTags;
        if (!request->dont_replicate()) {
            // Handling *empty* replicate_to_cell_tags has changed. Regardless of dont_replicate,
            // replication is skipped (well, more likely deferred). The "replicate to all cells"
            // behavior is no more (the config option to enable it will go away soon).
            //
            // This makes dont_replicate obsolete, and it will be removed in the future. For now,
            // it has to stay for compatibility.
            //
            // Other than that, we still obey replicate_to_cell_tags and do not attempt to be lazy
            // in this regard. This has two benefits:
            //   - it allows for better performance in certain cases;
            //   - it allows us to do without lazy transaction replication support in certain methods.
            //
            // One example of the latter is dyntable-related transactions. They specify target cells
            // explicitly, and this allows us, when registering a transaction action, to expect the
            // transaction to be present at the target cell immediately.

            replicateToCellTags = FromProto<TCellTagList>(request->replicate_to_cell_tags());

            if (!GetDynamicConfig()->EnableLazyTransactionReplication && replicateToCellTags.empty()) {
                const auto& multicellManager = Bootstrap_->GetMulticellManager();
                replicateToCellTags = multicellManager->GetRegisteredMasterCellTags();
            }
        }

       auto* transaction = StartTransaction(
            parent,
            prerequisiteTransactions,
            replicateToCellTags,
            timeout,
            deadline,
            title,
            *attributes);

        auto id = transaction->GetId();

        if (response) {
            ToProto(response->mutable_id(), id);
        }

        if (context) {
            context->SetResponseInfo("TransactionId: %v", id);
        }
    }

    void HydraStartForeignTransaction(NTransactionServer::NProto::TReqStartForeignTransaction* request)
    {
        auto hintId = FromProto<TTransactionId>(request->id());
        auto parentId = FromProto<TTransactionId>(request->parent_id());
        auto* parent = parentId ? FindTransaction(parentId) : nullptr;
        auto isUpload = request->upload();
        if (parentId && !parent) {
            THROW_ERROR_EXCEPTION("Failed to start foreign transaction: parent transaction not found")
                << TErrorAttribute("transaction_id", hintId)
                << TErrorAttribute("parent_transaction_id", parentId);
        }

        auto title = request->has_title() ? std::make_optional(request->title()) : std::nullopt;

        YT_VERIFY(
            !GetDynamicConfig()->EnableDedicatedUploadTransactionObjectTypes ||
            isUpload == (
                TypeFromId(hintId) == EObjectType::UploadTransaction ||
                TypeFromId(hintId) == EObjectType::UploadNestedTransaction));

        auto attributes = CreateEphemeralAttributes();
        if (request->has_operation_type()) {
            attributes->Set("operation_type", request->operation_type());
        }
        if (request->has_operation_id()) {
            attributes->Set("operation_id", request->operation_id());
        }
        if (request->has_operation_title()) {
            attributes->Set("operation_title", request->operation_title());
        }

        auto* transaction = DoStartTransaction(
            isUpload,
            parent,
            {} /*prerequisiteTransactions*/,
            {} /*replicatedToCellTags*/,
            std::nullopt /*timeout*/,
            std::nullopt /*deadline*/,
            title,
            *attributes,
            hintId);
        YT_VERIFY(transaction->GetId() == hintId);
    }

    TTransaction* ValidatePrerequisiteTransaction(TTransactionId transactionId)
    {
        auto* prerequisiteTransaction = FindTransaction(transactionId);
        if (!IsObjectAlive(prerequisiteTransaction)) {
            THROW_ERROR_EXCEPTION(NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: transaction %v is missing",
                transactionId);
        }
        if (prerequisiteTransaction->GetPersistentState() != ETransactionState::Active) {
            THROW_ERROR_EXCEPTION(NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: transaction %v is in %Qlv state",
                transactionId,
                prerequisiteTransaction->GetPersistentState());
        }

        return prerequisiteTransaction;
    }

    void HydraRegisterTransactionActions(
        const TCtxRegisterTransactionActionsPtr& /*context*/,
        TReqRegisterTransactionActions* request,
        TRspRegisterTransactionActions* /*response*/)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        auto* transaction = GetTransactionOrThrow(transactionId);

        auto state = transaction->GetPersistentState();
        if (state != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }

        for (const auto& protoData : request->actions()) {
            auto data = FromProto<TTransactionActionData>(protoData);
            transaction->Actions().push_back(data);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction action registered (TransactionId: %v, ActionType: %v)",
                transactionId,
                data.Type);
        }
    }

    void HydraPrepareTransactionCommit(NProto::TReqPrepareTransactionCommit* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto prepareTimestamp = request->prepare_timestamp();
        auto identity = NRpc::ParseAuthenticationIdentityFromProto(*request);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, std::move(identity));

        TTransactionPrepareOptions options{
            .Persistent = true,
            .PrepareTimestamp = prepareTimestamp,
        };
        PrepareTransactionCommit(transactionId, options);
    }

    void HydraCommitTransaction(NProto::TReqCommitTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = request->commit_timestamp();
        auto nativeCommitMutationRevision = request->native_commit_mutation_revision();

        TTransactionCommitOptions options{
            .CommitTimestamp = commitTimestamp,
        };
        CommitTransaction(transactionId, options, nativeCommitMutationRevision);
    }

    void HydraAbortTransaction(NProto::TReqAbortTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        TTransactionAbortOptions options{
            .Force = request->force(),
        };
        AbortTransaction(transactionId, options);
    }

    void HydraReplicateTransactions(
        const TCtxReplicateTransactionsPtr& context,
        TReqReplicateTransactions* request,
        TRspReplicateTransactions* response)
    {
        auto destinationCellTag = static_cast<TCellTag>(request->destination_cell_tag());

        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        TCompactVector<TTransactionId, 4> postedTransactionIds;
        TCompactVector<TTransactionId, 4> skippedTransactionIds;
        TCompactVector<TTransactionId, 4> postedMissingTransactionIds;
        for (const auto& protoTransactionId : request->transaction_ids()) {
            auto transactionId = FromProto<TTransactionId>(protoTransactionId);
            YT_VERIFY(CellTagFromId(transactionId) == Bootstrap_->GetCellTag());
            auto* transaction = FindTransaction(transactionId);

            if (!IsObjectAlive(transaction)) {
                NProto::TReqNoteNoSuchTransaction noSuchTransactionRequest;
                ToProto(noSuchTransactionRequest.mutable_id(), transactionId);
                multicellManager->PostToMaster(noSuchTransactionRequest, destinationCellTag);

                postedMissingTransactionIds.push_back(transactionId);

                continue;
            }

            YT_VERIFY(transaction->IsNative());

            if (transaction->IsReplicatedToCell(destinationCellTag)) {
                skippedTransactionIds.push_back(transactionId);
                // Don't post anything.
                continue;
            }

            auto replicatedTransactionId = ReplicateTransaction(transaction, {destinationCellTag});
            YT_VERIFY(replicatedTransactionId == transactionId);
            YT_VERIFY(transaction->IsReplicatedToCell(destinationCellTag));

            postedTransactionIds.push_back(transactionId);
        }

        response->set_sync_implied(!postedTransactionIds.empty());

        // NB: may be empty.
        auto boomerangWaveId = FromProto<TBoomerangWaveId>(request->boomerang_wave_id());
        YT_ASSERT(!boomerangWaveId ||
            request->has_boomerang_wave_id() &&
            request->has_boomerang_wave_size() &&
            request->has_boomerang_mutation_id() &&
            request->has_boomerang_mutation_type() &&
            request->has_boomerang_mutation_data());
        auto boomerangMutationId = request->has_boomerang_mutation_id()
            ? FromProto<NRpc::TMutationId>(request->boomerang_mutation_id())
            : NRpc::TMutationId();
        auto boomerangWaveSize = request->boomerang_wave_size();

        if (boomerangWaveId) {
            NProto::TReqReturnBoomerang boomerangRequest;

            boomerangRequest.mutable_boomerang_wave_id()->Swap(request->mutable_boomerang_wave_id());
            boomerangRequest.set_boomerang_wave_size(request->boomerang_wave_size());

            boomerangRequest.mutable_boomerang_mutation_id()->Swap(request->mutable_boomerang_mutation_id());
            boomerangRequest.set_boomerang_mutation_type(request->boomerang_mutation_type());
            boomerangRequest.set_boomerang_mutation_data(request->boomerang_mutation_data());

            multicellManager->PostToMaster(boomerangRequest, destinationCellTag);
        }

        if (context) {
            context->SetResponseInfo(
                "ReplicatedTransactionIds: %v, MissingTransactionIds: %v, SkippedTransactionIds: %v, "
                "BoomerangMutationId: %v, BoomerangWaveId: %v, BoomerangWaveSize: %v",
                postedTransactionIds,
                postedMissingTransactionIds,
                skippedTransactionIds,
                boomerangMutationId,
                boomerangWaveId,
                boomerangWaveSize);
        }
    }

    void HydraNoteNoSuchTransaction(NProto::TReqNoteNoSuchTransaction* request)
    {
        // NB: this has no effect on the persistent state, but it does notify
        // transient subscribers and does cache transaction absence.
        auto transactionId = FromProto<TTransactionId>(request->id());
        CacheTransactionFinished(transactionId);
    }

    void HydraReturnBoomerang(NProto::TReqReturnBoomerang* request)
    {
        BoomerangTracker_->ProcessReturnedBoomerang(request);
    }

    void HydraRemoveStuckBoomerangWaves(NProto::TReqRemoveStuckBoomerangWaves* request)
    {
        BoomerangTracker_->RemoveStuckBoomerangWaves(request);
    }

public:
    void FinishTransaction(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& objectManager = Bootstrap_->GetObjectManager();

        for (auto* object : transaction->StagedObjects()) {
            const auto& handler = objectManager->GetHandler(object);
            handler->UnstageObject(object, false);
            objectManager->UnrefObject(object);
        }
        transaction->StagedObjects().clear();

        for (auto* node : transaction->StagedNodes()) {
            objectManager->UnrefObject(node);
        }
        transaction->StagedNodes().clear();

        auto* parent = transaction->GetParent();
        if (parent) {
            YT_VERIFY(parent->NestedTransactions().erase(transaction) == 1);
            objectManager->UnrefObject(transaction);
            transaction->SetParent(nullptr);
        }

        if (transaction->IsNative()) {
            YT_VERIFY(NativeTransactions_.erase(transaction) == 1);
            if (!parent) {
                YT_VERIFY(NativeTopmostTransactions_.erase(transaction) == 1);
            }
        }

        for (auto* prerequisiteTransaction : transaction->PrerequisiteTransactions()) {
            // NB: Duplicates are fine; prerequisite transactions may be duplicated.
            prerequisiteTransaction->DependentTransactions().erase(transaction);
        }
        transaction->PrerequisiteTransactions().clear();

        TCompactVector<TTransaction*, 16> dependentTransactions(
            transaction->DependentTransactions().begin(),
            transaction->DependentTransactions().end());
        std::sort(dependentTransactions.begin(), dependentTransactions.end(), TObjectIdComparer());
        for (auto* dependentTransaction : dependentTransactions) {
            if (!IsObjectAlive(dependentTransaction)) {
                continue;
            }
            if (dependentTransaction->GetPersistentState() != ETransactionState::Active) {
                continue;
            }
            YT_LOG_DEBUG("Aborting dependent transaction (DependentTransactionId: %v, PrerequisiteTransactionId: %v)",
                dependentTransaction->GetId(),
                transaction->GetId());
            TTransactionAbortOptions options{
                .Force = true,
            };
            AbortTransaction(dependentTransaction, options, /*validatePermissions*/ false);
        }
        transaction->DependentTransactions().clear();

        transaction->SetDeadline(std::nullopt);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ResetTransactionAccountResourceUsage(transaction);

        CacheTransactionFinished(transaction);

        // Kill the fake reference thus destroying the object.
        objectManager->UnrefObject(transaction);
    }
private:

    // Cf. TTransactionPresenceCache::GetTransactionPresence
    bool ShouldCacheTransactionPresence(TTransaction* transaction)
    {
        YT_ASSERT(TypeFromId(transaction->GetId()) == transaction->GetType());
        return ShouldCacheTransactionPresence(transaction->GetId());
    }

    bool ShouldCacheTransactionPresence(TTransactionId transactionId)
    {
        auto transactionType = TypeFromId(transactionId);
        // NB: if enable_dedicated_upload_transaction_object_types is false,
        // upload transactions *will* be cached.
        if (transactionType == EObjectType::UploadTransaction ||
            transactionType == EObjectType::UploadNestedTransaction)
        {
            return false;
        }

        if (CellTagFromId(transactionId) == Bootstrap_->GetCellTag()) {
            return false;
        }

        return true;
    }

    void CacheTransactionStarted(TTransaction* transaction)
    {
        if (ShouldCacheTransactionPresence(transaction)) {
            TransactionPresenceCache_->SetTransactionReplicated(transaction->GetId());
        }
    }

    void CacheTransactionFinished(TTransaction* transaction)
    {
        if (ShouldCacheTransactionPresence(transaction)) {
            TransactionPresenceCache_->SetTransactionRecentlyFinished(transaction->GetId());
        }
    }

    void CacheTransactionFinished(TTransactionId transactionId)
    {
        if (ShouldCacheTransactionPresence(transactionId)) {
            TransactionPresenceCache_->SetTransactionRecentlyFinished(transactionId);
        }
    }


    void SaveKeys(NCellMaster::TSaveContext& context)
    {
        TransactionMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context)
    {
        using NYT::Save;

        TransactionMap_.SaveValues(context);
        Save(context, TimestampHolderMap_);
        BoomerangTracker_->Save(context);
    }

    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TransactionMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Load;

        TransactionMap_.LoadValues(context);
        Load(context, TimestampHolderMap_);
        BoomerangTracker_->Load(context);
    }


    void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Reconstruct NativeTransactions and NativeTopmostTransactions.
        for (auto [id, transaction] : TransactionMap_) {
            if (!IsObjectAlive(transaction)) {
                continue;
            }

            if (transaction->IsNative()) {
                YT_VERIFY(NativeTransactions_.insert(transaction).second);
                if (!transaction->GetParent()) {
                    YT_VERIFY(NativeTopmostTransactions_.insert(transaction).second);
                }
            }
        }

        // Fill transaction presence cache.
        for (auto [id, transaction] : TransactionMap_) {
            if (IsObjectAlive(transaction)) {
                CacheTransactionStarted(transaction);
            }
        }
    }

    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        TransactionMap_.Clear();
        NativeTopmostTransactions_.clear();
        NativeTransactions_.clear();
        TransactionPresenceCache_->Clear();
    }

    void OnStartLeading() override
    {
        TMasterAutomatonPart::OnStartLeading();

        OnStartEpoch();
    }

    void OnStartFollowing() override
    {
        TMasterAutomatonPart::OnStartFollowing();

        OnStartEpoch();
    }

    void OnStartEpoch()
    {
        TransactionPresenceCache_->Start();
    }

    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        LeaseTracker_->Start();

        // Recreate leases for all active transactions.
        for (auto [transactionId, transaction] : TransactionMap_) {
            auto state = transaction->GetTransientState();
            if (state == ETransactionState::Active ||
                state == ETransactionState::PersistentCommitPrepared)
            {
                CreateLease(transaction);
            }
        }

        BoomerangTracker_->Start();
    }

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        BoomerangTracker_->Stop();
        LeaseTracker_->Stop();

        // Reset all transiently prepared transactions back into active state.
        for (auto [transactionId, transaction] : TransactionMap_) {
            transaction->ResetTransientState();
        }

        OnStopEpoch();
    }

    void OnStopFollowing() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopFollowing();

        OnStopEpoch();
    }

    void OnStopEpoch()
    {
        TransactionPresenceCache_->Stop();
    }

    void OnRecoveryStarted() override
    {
        TMasterAutomatonPart::OnRecoveryStarted();

        BufferedProducer_->SetEnabled(false);
    }

    void OnRecoveryComplete() override
    {
        TMasterAutomatonPart::OnRecoveryComplete();

        BufferedProducer_->SetEnabled(true);
    }

    void CreateLease(TTransaction* transaction)
    {
        const auto& hydraFacade = Bootstrap_->GetHydraFacade();
        LeaseTracker_->RegisterTransaction(
            transaction->GetId(),
            GetObjectId(transaction->GetParent()),
            transaction->GetTimeout(),
            transaction->GetDeadline(),
            BIND(&TImpl::OnTransactionExpired, MakeStrong(this))
                .Via(hydraFacade->GetEpochAutomatonInvoker(EAutomatonThreadQueue::TransactionSupervisor)));
    }

    void CloseLease(TTransaction* transaction)
    {
        LeaseTracker_->UnregisterTransaction(transaction->GetId());
    }

    void OnTransactionExpired(TTransactionId transactionId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = FindTransaction(transactionId);
        if (!IsObjectAlive(transaction)) {
            return;
        }
        if (transaction->GetTransientState() != ETransactionState::Active) {
            return;
        }

        const auto& transactionSupervisor = Bootstrap_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(transactionId)
            .Subscribe(BIND([=, this, this_ = MakeStrong(this)] (const TError& error) {
                if (!error.IsOK()) {
                    YT_LOG_DEBUG(error, "Error aborting expired transaction (TransactionId: %v)",
                        transactionId);
                }
            }));
    }

    std::unique_ptr<TSequoiaContextGuard> CreateSequoiaContextGuard(TTransaction* transaction)
    {
        if (transaction->GetIsSequoiaTransaction()) {
            auto sequoiaContext = CreateSequoiaContext(Bootstrap_, transaction->GetId(), transaction->SequoiaWriteSet());
            return std::make_unique<TSequoiaContextGuard>(std::move(sequoiaContext));
        } else {
            return nullptr;
        }
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (TransactionPresenceCache_) {
            TSensorBuffer buffer;

            buffer.AddGauge("/cached_replicated_transaction_count", TransactionPresenceCache_->GetReplicatedTransactionCount());
            buffer.AddGauge("/cached_recently_finished_transaction_count", TransactionPresenceCache_->GetRecentlyFinishedTransactionCount());
            buffer.AddGauge("/subscribed_remote_transaction_replication_count", TransactionPresenceCache_->GetSubscribedRemoteTransactionReplicationCount());

            BufferedProducer_->Update(std::move(buffer));
        }
    }

    const TDynamicTransactionManagerConfigPtr& GetDynamicConfig()
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->TransactionManager;
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/ = nullptr)
    {
        ProfilingExecutor_->SetPeriod(GetDynamicConfig()->ProfilingPeriod);
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTransactionManager::TImpl, Transaction, TTransaction, TransactionMap_)

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionTypeHandler::TTransactionTypeHandler(
    TImpl* owner,
    EObjectType objectType)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->TransactionMap_)
    , ObjectType_(objectType)
{ }

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionManager(TBootstrap* bootstrap)
    : Impl_(New<TImpl>(bootstrap))
{ }

TTransactionManager::~TTransactionManager()
{ }

void TTransactionManager::Initialize()
{
    Impl_->Initialize();
}

TTransaction* TTransactionManager::StartTransaction(
    TTransaction* parent,
    std::vector<TTransaction*> prerequisiteTransactions,
    const TCellTagList& replicatedToCellTags,
    std::optional<TDuration> timeout,
    std::optional<TInstant> deadline,
    const std::optional<TString>& title,
    const IAttributeDictionary& attributes,
    TTransactionId hintId)
{
    return Impl_->StartTransaction(
        parent,
        std::move(prerequisiteTransactions),
        replicatedToCellTags,
        timeout,
        deadline,
        title,
        attributes,
        hintId);
}

TTransaction* TTransactionManager::StartUploadTransaction(
    TTransaction* parent,
    const TCellTagList& replicatedToCellTags,
    std::optional<TDuration> timeout,
    const std::optional<TString>& title,
    TTransactionId hintId)
{
    return Impl_->StartUploadTransaction(
        parent,
        replicatedToCellTags,
        timeout,
        title,
        hintId);
}

void TTransactionManager::CommitTransaction(
    TTransaction* transaction,
    const TTransactionCommitOptions& options)
{
    Impl_->CommitTransaction(transaction, options);
}

void TTransactionManager::AbortTransaction(
    TTransaction* transaction,
    const TTransactionAbortOptions& options)
{
    Impl_->AbortTransaction(transaction, options);
}

TTransactionId TTransactionManager::ExternalizeTransaction(TTransaction* transaction, TCellTagList dstCellTags)
{
    return Impl_->ExternalizeTransaction(transaction, dstCellTags);
}

TTransactionId TTransactionManager::GetNearestExternalizedTransactionAncestor(
    TTransaction* transaction,
    TCellTag dstCellTag)
{
    return Impl_->GetNearestExternalizedTransactionAncestor(transaction, dstCellTag);
}

TTransaction* TTransactionManager::GetTransactionOrThrow(TTransactionId transactionId)
{
    return Impl_->GetTransactionOrThrow(transactionId);
}

TFuture<TInstant> TTransactionManager::GetLastPingTime(const TTransaction* transaction)
{
    return Impl_->GetLastPingTime(transaction);
}

void TTransactionManager::SetTransactionTimeout(
    TTransaction* transaction,
    TDuration timeout)
{
    Impl_->SetTransactionTimeout(transaction, timeout);
}

void TTransactionManager::StageObject(
    TTransaction* transaction,
    TObject* object)
{
    Impl_->StageObject(transaction, object);
}

void TTransactionManager::UnstageObject(
    TTransaction* transaction,
    TObject* object,
    bool recursive)
{
    Impl_->UnstageObject(transaction, object, recursive);
}

void TTransactionManager::StageNode(
    TTransaction* transaction,
    TCypressNode* trunkNode)
{
    Impl_->StageNode(transaction, trunkNode);
}

void TTransactionManager::ExportObject(
    TTransaction* transaction,
    TObject* object,
    TCellTag destinationCellTag)
{
    Impl_->ExportObject(transaction, object, destinationCellTag);
}

void TTransactionManager::ImportObject(
    TTransaction* transaction,
    TObject* object)
{
    Impl_->ImportObject(transaction, object);
}

void TTransactionManager::RegisterTransactionActionHandlers(
    const TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
    const TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
    const TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor)
{
    Impl_->RegisterTransactionActionHandlers(
        prepareActionDescriptor,
        commitActionDescriptor,
        abortActionDescriptor);
}

std::unique_ptr<TMutation> TTransactionManager::CreateStartTransactionMutation(
    TCtxStartTransactionPtr context,
    const NProto::TReqStartTransaction& request)
{
    return Impl_->CreateStartTransactionMutation(std::move(context), request);
}

std::unique_ptr<TMutation> TTransactionManager::CreateRegisterTransactionActionsMutation(TCtxRegisterTransactionActionsPtr context)
{
    return Impl_->CreateRegisterTransactionActionsMutation(std::move(context));
}

std::unique_ptr<TMutation> TTransactionManager::CreateReplicateTransactionsMutation(TCtxReplicateTransactionsPtr context)
{
    return Impl_->CreateReplicateTransactionsMutation(std::move(context));
}

TFuture<void> TTransactionManager::GetReadyToPrepareTransactionCommit(
    const std::vector<TTransactionId>& prerequisiteTransactionIds,
    const std::vector<TCellId>& cellIdsToSyncWith)
{
    return Impl_->GetReadyToPrepareTransactionCommit(
        prerequisiteTransactionIds,
        cellIdsToSyncWith);
}

void TTransactionManager::PrepareTransactionCommit(
    TTransactionId transactionId,
    const TTransactionPrepareOptions& options)
{
    Impl_->PrepareTransactionCommit(transactionId, options);
}

void TTransactionManager::PrepareTransactionAbort(
    TTransactionId transactionId,
    const TTransactionAbortOptions& options)
{
    Impl_->PrepareTransactionAbort(transactionId, options);
}

void TTransactionManager::CommitTransaction(
    TTransactionId transactionId,
    const TTransactionCommitOptions& options)
{
    Impl_->CommitTransaction(transactionId, options, /*nativeCommitMutationRevision*/ NullRevision);
}

void TTransactionManager::AbortTransaction(
    TTransactionId transactionId,
    const TTransactionAbortOptions& options)
{
    Impl_->AbortTransaction(transactionId, options);
}

void TTransactionManager::PingTransaction(
    TTransactionId transactionId,
    bool pingAncestors)
{
    Impl_->PingTransaction(transactionId, pingAncestors);
}

void TTransactionManager::CreateOrRefTimestampHolder(TTransactionId transactionId)
{
    Impl_->CreateOrRefTimestampHolder(transactionId);
}

void TTransactionManager::SetTimestampHolderTimestamp(TTransactionId transactionId, TTimestamp timestamp)
{
    Impl_->SetTimestampHolderTimestamp(transactionId, timestamp);
}

TTimestamp TTransactionManager::GetTimestampHolderTimestamp(TTransactionId transactionId)
{
    return Impl_->GetTimestampHolderTimestamp(transactionId);
}

void TTransactionManager::UnrefTimestampHolder(TTransactionId transactionId)
{
    Impl_->UnrefTimestampHolder(transactionId);
}

const TTransactionPresenceCachePtr& TTransactionManager::GetTransactionPresenceCache()
{
    return Impl_->GetTransactionPresenceCache();
}

DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionStarted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionCommitted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionAborted, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TTransactionManager, THashSet<TTransaction*>, NativeTopmostTransactions, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TTransactionManager, THashSet<TTransaction*>, NativeTransactions, *Impl_);
DELEGATE_ENTITY_MAP_ACCESSORS(TTransactionManager, Transaction, TTransaction, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
