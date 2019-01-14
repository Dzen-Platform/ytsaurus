#include "client_impl.h"
#include "helpers.h"
#include "config.h"
#include "transaction.h"
#include "private.h"
#include "table_mount_cache.h"
#include "timestamp_provider.h"
#include "credentials_injecting_channel.h"
#include "dynamic_channel_pool.h"

#include <yt/core/net/address.h>

#include <yt/core/misc/small_set.h>

#include <yt/core/ytree/convert.h>

#include <yt/client/api/rowset.h>
#include <yt/client/api/transaction.h>

#include <yt/client/transaction_client/remote_timestamp_provider.h>

#include <yt/client/scheduler/operation_id_or_alias.h>

#include <yt/client/table_client/unversioned_row.h>
#include <yt/client/table_client/row_base.h>
#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/wire_protocol.h>

#include <yt/client/tablet_client/table_mount_cache.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

using NYT::ToProto;
using NYT::FromProto;

using namespace NRpc;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NScheduler;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

IChannelPtr CreateCredentialsInjectingChannel(
    IChannelPtr underlying,
    const TClientOptions& options)
{
    if (options.Token) {
        return CreateTokenInjectingChannel(
            underlying,
            options.PinnedUser,
            *options.Token);
    } else if (options.SessionId || options.SslSessionId) {
        return CreateCookieInjectingChannel(
            underlying,
            options.PinnedUser,
            options.SessionId.value_or(TString()),
            options.SslSessionId.value_or(TString()));
    } else {
        return CreateUserInjectingChannel(underlying, options.PinnedUser);
    }
}

////////////////////////////////////////////////////////////////////////////////

TClient::TClient(
    TConnectionPtr connection,
    TDynamicChannelPoolPtr channelPool,
    const TClientOptions& clientOptions)
    : Connection_(std::move(connection))
    , ChannelPool_(channelPool)
    , Channel_(CreateCredentialsInjectingChannel(CreateDynamicChannel(channelPool), clientOptions))
    , ClientOptions_(clientOptions)
{ }

const ITableMountCachePtr& TClient::GetTableMountCache()
{
    if (!TableMountCacheInitialized_.load()) {
        auto guard = Guard(TableMountCacheSpinLock_);
        if (!TableMountCache_) {
            TableMountCache_ = CreateTableMountCache(
                Connection_->GetConfig()->TableMountCache,
                Channel_,
                RpcProxyClientLogger,
                Connection_->GetConfig()->RpcTimeout);
        }
        TableMountCacheInitialized_.store(true);
    }
    return TableMountCache_;
}

const ITimestampProviderPtr& TClient::GetTimestampProvider()
{
    if (!TimestampProviderInitialized_.load()) {
        auto guard = Guard(TimestampProviderSpinLock_);
        if (!TimestampProvider_) {
            TimestampProvider_ = CreateBatchingTimestampProvider(
                CreateTimestampProvider(Channel_, Connection_->GetConfig()->RpcTimeout),
                Connection_->GetConfig()->TimestampProviderUpdatePeriod);
        }
        TimestampProviderInitialized_.store(true);
    }
    return TimestampProvider_;
}

TFuture<void> TClient::Terminate()
{
    return VoidFuture;
}

TConnectionPtr TClient::GetRpcProxyConnection()
{
    return Connection_;
}

TClientPtr TClient::GetRpcProxyClient()
{
    return this;
}

IChannelPtr TClient::GetChannel()
{
    return Channel_;
}

IChannelPtr TClient::GetStickyChannel()
{
    return CreateCredentialsInjectingChannel(
        CreateStickyChannel(ChannelPool_),
        ClientOptions_);
}

ITransactionPtr TClient::AttachTransaction(
    TTransactionId transactionId,
    const TTransactionAttachOptions& options)
{
    if (options.Sticky) {
        THROW_ERROR_EXCEPTION("Attaching to sticky transactions is not supported");
    }
    auto connection = GetRpcProxyConnection();
    auto client = GetRpcProxyClient();
    auto channel = GetChannel();

    TApiServiceProxy proxy(channel);

    auto req = proxy.AttachTransaction();
    ToProto(req->mutable_transaction_id(), transactionId);
    req->set_sticky(options.Sticky);
    if (options.PingPeriod) {
        req->set_ping_period(options.PingPeriod->GetValue());
    }
    req->set_ping(options.Ping);
    req->set_ping_ancestors(options.PingAncestors);

    auto rspOrError = NConcurrency::WaitFor(req->Invoke());
    const auto& rsp = rspOrError.ValueOrThrow();
    auto transactionType = static_cast<ETransactionType>(rsp->type());
    auto startTimestamp = static_cast<TTimestamp>(rsp->start_timestamp());
    auto atomicity = static_cast<EAtomicity>(rsp->atomicity());
    auto durability = static_cast<EDurability>(rsp->durability());
    auto timeout = TDuration::FromValue(NYT::FromProto<i64>(rsp->timeout()));

    return CreateTransaction(
        std::move(connection),
        std::move(client),
        std::move(channel),
        transactionId,
        startTimestamp,
        transactionType,
        atomicity,
        durability,
        timeout,
        options.PingPeriod,
        options.Sticky);
}

TFuture<void> TClient::MountTable(
    const TYPath& path,
    const TMountTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.MountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    NYT::ToProto(req->mutable_cell_id(), options.CellId);
    if (!options.TargetCellIds.empty()) {
        NYT::ToProto(req->mutable_target_cell_ids(), options.TargetCellIds);
    }
    req->set_freeze(options.Freeze);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UnmountTable(
    const TYPath& path,
    const TUnmountTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.UnmountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    req->set_force(options.Force);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::RemountTable(
    const TYPath& path,
    const TRemountTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.RemountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::FreezeTable(
    const TYPath& path,
    const TFreezeTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.FreezeTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UnfreezeTable(
    const TYPath& path,
    const TUnfreezeTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.UnfreezeTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ReshardTable(
    const TYPath& path,
    const std::vector<NTableClient::TOwningKey>& pivotKeys,
    const TReshardTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.ReshardTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    TWireProtocolWriter writer;
    // XXX(sandello): This is ugly and inefficient.
    std::vector<TUnversionedRow> keys;
    for (const auto& pivotKey : pivotKeys) {
        keys.push_back(pivotKey);
    }
    writer.WriteRowset(MakeRange(keys));
    req->Attachments() = writer.Finish();

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ReshardTable(
    const TYPath& path,
    int tabletCount,
    const TReshardTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.ReshardTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_tablet_count(tabletCount);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<std::vector<TTabletActionId>> TClient::ReshardTableAutomatic(
    const TYPath& path,
    const TReshardTableAutomaticOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.ReshardTableAutomatic();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_keep_actions(options.KeepActions);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspReshardTableAutomaticPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTabletActionId>>(rsp->tablet_actions());
    }));
}

TFuture<void> TClient::TrimTable(
    const TYPath& path,
    int tabletIndex,
    i64 trimmedRowCount,
    const TTrimTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.TrimTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_tablet_index(tabletIndex);
    req->set_trimmed_row_count(trimmedRowCount);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AlterTable(
    const TYPath& path,
    const TAlterTableOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AlterTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    if (options.Schema) {
        req->set_schema(ConvertToYsonString(*options.Schema).GetData());
    }
    if (options.Dynamic) {
        req->set_dynamic(*options.Dynamic);
    }
    if (options.UpstreamReplicaId) {
        ToProto(req->mutable_upstream_replica_id(), *options.UpstreamReplicaId);
    }

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_transactional_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AlterTableReplica(
    TTableReplicaId replicaId,
    const TAlterTableReplicaOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AlterTableReplica();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_replica_id(), replicaId);

    if (options.Enabled) {
        req->set_enabled(*options.Enabled);
    }
    if (options.Mode) {
        req->set_mode(static_cast<NProto::ETableReplicaMode>(*options.Mode));
    }
    if (options.PreserveTimestamps) {
        req->set_preserve_timestamps(*options.PreserveTimestamps);
    }
    if (options.Atomicity) {
        req->set_atomicity(static_cast<NProto::EAtomicity>(*options.Atomicity));
    }

    return req->Invoke().As<void>();
}

TFuture<std::vector<TTableReplicaId>> TClient::GetInSyncReplicas(
    const TYPath& path,
    TNameTablePtr nameTable,
    const TSharedRange<NTableClient::TKey>& keys,
    const TGetInSyncReplicasOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.GetInSyncReplicas();
    SetTimeoutOptions(*req, options);

    if (options.Timestamp) {
        req->set_timestamp(options.Timestamp);
    }

    req->set_path(path);
    req->Attachments() = SerializeRowset(nameTable, keys, req->mutable_rowset_descriptor());

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspGetInSyncReplicasPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTableReplicaId>>(rsp->replica_ids());
    }));
}

TFuture<std::vector<NApi::TTabletInfo>> TClient::GetTabletInfos(
    const TYPath& path,
    const std::vector<int>& tabletIndexes,
    const TGetTabletsInfoOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.GetTabletInfos();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    ToProto(req->mutable_tablet_indexes(), tabletIndexes);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspGetTabletInfosPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        std::vector<NApi::TTabletInfo> tabletInfos;
        tabletInfos.reserve(rsp->tablets_size());
        for (const auto& protoTabletInfo : rsp->tablets()) {
            tabletInfos.emplace_back();
            auto& result = tabletInfos.back();
            result.TotalRowCount = protoTabletInfo.total_row_count();
            result.TrimmedRowCount = protoTabletInfo.trimmed_row_count();
        }
        return tabletInfos;
    }));
}

TFuture<std::vector<TTabletActionId>> TClient::BalanceTabletCells(
    const TString& tabletCellBundle,
    const std::vector<NYPath::TYPath>& movableTables,
    const NApi::TBalanceTabletCellsOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.BalanceTabletCells();
    SetTimeoutOptions(*req, options);

    req->set_bundle(tabletCellBundle);
    req->set_keep_actions(options.KeepActions);
    ToProto(req->mutable_movable_tables(), movableTables);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspBalanceTabletCellsPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTabletActionId>>(rsp->tablet_actions());
    }));
}

TFuture<void> TClient::AddMember(
    const TString& group,
    const TString& member,
    const NApi::TAddMemberOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AddMember();
    SetTimeoutOptions(*req, options);

    req->set_group(group);
    req->set_member(member);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::RemoveMember(
    const TString& group,
    const TString& member,
    const NApi::TRemoveMemberOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.RemoveMember();
    SetTimeoutOptions(*req, options);

    req->set_group(group);
    req->set_member(member);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().As<void>();
}

TFuture<NApi::TCheckPermissionResult> TClient::CheckPermission(
    const TString& user,
    const NYPath::TYPath& path,
    NYTree::EPermission permission,
    const NApi::TCheckPermissionOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.CheckPermission();
    SetTimeoutOptions(*req, options);

    req->set_user(user);
    req->set_path(path);
    req->set_permission(static_cast<int>(permission));

    ToProto(req->mutable_master_read_options(), options);
    ToProto(req->mutable_transactional_options(), options);
    ToProto(req->mutable_prerequisite_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspCheckPermissionPtr& rsp) {
        return FromProto<NApi::TCheckPermissionResult>(rsp->result());
    }));
}

TFuture<NScheduler::TOperationId> TClient::StartOperation(
    NScheduler::EOperationType type,
    const NYson::TYsonString& spec,
    const NApi::TStartOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.StartOperation();
    SetTimeoutOptions(*req, options);

    req->set_type(NProto::ConvertOperationTypeToProto(type));
    req->set_spec(spec.GetData());

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_transactional_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspStartOperationPtr& rsp) {
        return FromProto<NScheduler::TOperationId>(rsp->operation_id());
    }));
}

TFuture<void> TClient::AbortOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TAbortOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AbortOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    if (options.AbortMessage) {
        req->set_abort_message(*options.AbortMessage);
    }

    return req->Invoke().As<void>();
}

TFuture<void> TClient::SuspendOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TSuspendOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.SuspendOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);
    req->set_abort_running_jobs(options.AbortRunningJobs);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ResumeOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TResumeOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.ResumeOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::CompleteOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TCompleteOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.CompleteOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UpdateOperationParameters(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NYson::TYsonString& parameters,
    const NApi::TUpdateOperationParametersOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.UpdateOperationParameters();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    req->set_parameters(parameters.GetData());

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::GetOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TGetOperationOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.GetOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    ToProto(req->mutable_master_read_options(), options);
    if (options.Attributes) {
        for (const auto& attribute : *options.Attributes) {
            req->add_attributes(attribute);
        }
    }
    req->set_include_runtime(options.IncludeRuntime);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetOperationPtr& rsp) {
        return NYson::TYsonString(rsp->meta());
    }));
}

TFuture<void> TClient::DumpJobContext(
    NJobTrackerClient::TJobId jobId,
    const NYPath::TYPath& path,
    const NApi::TDumpJobContextOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.DumpJobContext();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_path(path);

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::GetJob(
    NJobTrackerClient::TOperationId operationId,
    NJobTrackerClient::TJobId jobId,
    const NApi::TGetJobOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.GetJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_operation_id(), operationId);
    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetJobPtr& rsp) {
        return NYson::TYsonString(rsp->info());
    }));
}

TFuture<NYson::TYsonString> TClient::StraceJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TStraceJobOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.StraceJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspStraceJobPtr& rsp) {
        return NYson::TYsonString(rsp->trace());
    }));
}

TFuture<void> TClient::SignalJob(
    NJobTrackerClient::TJobId jobId,
    const TString& signalName,
    const NApi::TSignalJobOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.SignalJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_signal_name(signalName);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AbandonJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TAbandonJobOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AbandonJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::PollJobShell(
    NJobTrackerClient::TJobId jobId,
    const NYson::TYsonString& parameters,
    const NApi::TPollJobShellOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.PollJobShell();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_parameters(parameters.GetData());

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspPollJobShellPtr& rsp) {
        return NYson::TYsonString(rsp->result());
    }));
}

TFuture<void> TClient::AbortJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TAbortJobOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.AbortJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    if (options.InterruptTimeout) {
        req->set_interrupt_timeout(NYT::ToProto<i64>(*options.InterruptTimeout));
    }

    return req->Invoke().As<void>();
}
////////////////////////////////////////////////////////////////////////////////

TFuture<NApi::TGetFileFromCacheResult> TClient::GetFileFromCache(
    const TString& md5,
    const NApi::TGetFileFromCacheOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.GetFileFromCache();
    SetTimeoutOptions(*req, options);

    req->set_md5(md5);
    req->set_cache_path(options.CachePath);

    ToProto(req->mutable_master_read_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetFileFromCachePtr& rsp) {
        return FromProto<NApi::TGetFileFromCacheResult>(rsp->result());
    }));
}

////////////////////////////////////////////////////////////////////////////////

TFuture<NApi::TPutFileToCacheResult> TClient::PutFileToCache(
    const NYPath::TYPath& path,
    const TString& expectedMD5,
    const NApi::TPutFileToCacheOptions& options)
{
    TApiServiceProxy proxy(GetChannel());

    auto req = proxy.PutFileToCache();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_md5(expectedMD5);
    req->set_cache_path(options.CachePath);

    ToProto(req->mutable_prerequisite_options(), options);
    ToProto(req->mutable_master_read_options(), options);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspPutFileToCachePtr& rsp) {
        return FromProto<NApi::TPutFileToCacheResult>(rsp->result());
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
