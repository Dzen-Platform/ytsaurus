#pragma once

#include "public.h"

#include <yt/ytlib/transaction_client/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/api/native/public.h>

#include <yt/core/ytree/public.h>
#include <yt/core/ytree/permission.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/logging/log.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

NYPath::TYPath GetOperationsPath();
NYPath::TYPath GetOperationPath(TOperationId operationId);
NYPath::TYPath GetJobsPath(TOperationId operationId);
NYPath::TYPath GetJobPath(TOperationId operationId, TJobId jobId);
NYPath::TYPath GetStderrPath(TOperationId operationId, TJobId jobId);
NYPath::TYPath GetSnapshotPath(TOperationId operationId);
NYPath::TYPath GetSecureVaultPath(TOperationId operationId);
NYPath::TYPath GetFailContextPath(TOperationId operationId, TJobId jobId);

NYPath::TYPath GetSchedulerOrchidOperationPath(TOperationId operationId);
NYPath::TYPath GetSchedulerOrchidAliasPath(const TString& alias);
NYPath::TYPath GetControllerAgentOrchidOperationPath(
    const TString& controllerAgentAddress,
    TOperationId operationId);
std::optional<TString> GetControllerAgentAddressFromCypress(
    TOperationId operationId,
    const NRpc::IChannelPtr& channel);

NYPath::TYPath GetJobPath(
    TOperationId operationId,
    TJobId jobId,
    const TString& resourceName);

const NYPath::TYPath& GetPoolTreesPath();
const NYPath::TYPath& GetOperationsArchiveOrderedByIdPath();
const NYPath::TYPath& GetOperationsArchiveOperationAliasesPath();
const NYPath::TYPath& GetOperationsArchiveOrderedByStartTimePath();
const NYPath::TYPath& GetOperationsArchiveVersionPath();
const NYPath::TYPath& GetOperationsArchiveJobsPath();
const NYPath::TYPath& GetOperationsArchiveJobSpecsPath();
const NYPath::TYPath& GetOperationsArchiveJobStderrsPath();
const NYPath::TYPath& GetOperationsArchiveJobProfilesPath();
const NYPath::TYPath& GetOperationsArchiveJobFailContextsPath();

bool IsOperationFinished(EOperationState state);
bool IsOperationFinishing(EOperationState state);
bool IsOperationInProgress(EOperationState state);

void ValidateEnvironmentVariableName(TStringBuf name);
bool IsOperationWithUserJobs(EOperationType operationType);

int GetJobSpecVersion();

bool IsSchedulingReason(EAbortReason reason);
bool IsNonSchedulingReason(EAbortReason reason);
bool IsSentinelReason(EAbortReason reason);

TError GetSchedulerTransactionsAbortedError(const std::vector<NObjectClient::TTransactionId>& transactionIds);
TError GetUserTransactionAbortedError(NObjectClient::TTransactionId transactionId);

////////////////////////////////////////////////////////////////////////////////

struct TJobFile
{
    TJobId JobId;
    NYPath::TYPath Path;
    NChunkClient::TChunkId ChunkId;
    TString DescriptionType;
};

void SaveJobFiles(NApi::NNative::IClientPtr client, TOperationId operationId, const std::vector<TJobFile>& files);

////////////////////////////////////////////////////////////////////////////////

//! Validate that given user has permission to an operation node of a given operation.
//! If needed, access to a certain subnode may be checked, not to the whole operation node.
void ValidateOperationPermission(
    const TString& user,
    TOperationId operationId,
    const NApi::IClientPtr& client,
    NYTree::EPermission permission,
    const NLogging::TLogger& logger,
    const TString& subnodePath = "");

void BuildOperationAce(
    const std::vector<TString>& owners,
    const TString& authenticatedUser,
    NYTree::EPermissionSet permissions,
    NYTree::TFluentList fluent);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
