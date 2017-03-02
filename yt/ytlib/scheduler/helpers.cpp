#include "helpers.h"

#include <yt/core/misc/error.h>
#include <yt/core/ypath/token.h>

#include <util/string/ascii.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYPath;

////////////////////////////////////////////////////////////////////

TYPath GetOperationPath(const TOperationId& operationId)
{
    return
        "//sys/operations/" +
        ToYPathLiteral(ToString(operationId));
}

TYPath GetJobsPath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId) +
        "/jobs";
}

TYPath GetJobPath(const TOperationId& operationId, const TJobId& jobId)
{
    return
        GetJobsPath(operationId) + "/" +
        ToYPathLiteral(ToString(jobId));
}

TYPath GetStderrPath(const TOperationId& operationId, const TJobId& jobId)
{
    return
        GetJobPath(operationId, jobId)
        + "/stderr";
}

TYPath GetFailContextPath(const TOperationId& operationId, const TJobId& jobId)
{
    return
        GetJobPath(operationId, jobId)
        + "/fail_context";
}

TYPath GetSnapshotPath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/snapshot";
}

TYPath GetSecureVaultPath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/secure_vault";
}

TYPath GetPoolsPath()
{
    return "//sys/pools";
}

TYPath GetLivePreviewOutputPath(const TOperationId& operationId, int tableIndex)
{
    return
        GetOperationPath(operationId)
        + "/output_" + ToString(tableIndex);
}

TYPath GetLivePreviewStderrTablePath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/stderr";
}

TYPath GetLivePreviewIntermediatePath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/intermediate";
}

TYPath GetOperationsArchiveJobsPath()
{
    return "//sys/operations_archive/jobs";
}

bool IsOperationFinished(EOperationState state)
{
    return
        state == EOperationState::Completed ||
        state == EOperationState::Aborted ||
        state == EOperationState::Failed;
}

bool IsOperationFinishing(EOperationState state)
{
    return
        state == EOperationState::Completing ||
        state == EOperationState::Aborting ||
        state == EOperationState::Failing;
}

bool IsOperationInProgress(EOperationState state)
{
    return
        state == EOperationState::Initializing ||
        state == EOperationState::Preparing ||
        state == EOperationState::Materializing ||
        state == EOperationState::Pending ||
        state == EOperationState::Reviving ||
        state == EOperationState::Running ||
        state == EOperationState::Completing ||
        state == EOperationState::Failing ||
        state == EOperationState::Aborting;
}

void ValidateEnvironmentVariableName(const TStringBuf& name)
{
    static const int MaximumNameLength = 1 << 16; // 64 kilobytes.
    if (name.size() > MaximumNameLength) {
        THROW_ERROR_EXCEPTION("Maximum length of the name for an environment variable violated: %v > %v",
            name.size(),
            MaximumNameLength);
    }
    for (char c : name) {
        if (!IsAsciiAlnum(c) && c != '_') {
            THROW_ERROR_EXCEPTION("Only alphanumeric characters and underscore are allowed in environment variable names")
                << TErrorAttribute("name", name);
        }
    }
}

int GetJobSpecVersion()
{
    return 1;
}

bool IsSchedulingReason(EAbortReason reason)
{
    return reason > EAbortReason::SchedulingBeginMarker && reason < EAbortReason::SchedulingEndMarker;
}

bool IsNonSchedulingReason(EAbortReason reason)
{
    return reason < EAbortReason::SchedulingBeginMarker;
}

bool IsMarker(EAbortReason reason)
{
    return reason == EAbortReason::SchedulingBeginMarker ||
        reason == EAbortReason::SchedulingEndMarker;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

