#pragma once

#include "private.h"

#include <yt/ytlib/hive/cluster_directory.h>
#include <yt/ytlib/hive/public.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/public.h>

#include <yt/core/yson/public.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct IJobHost
    : public TIntrinsicRefCounted
{
    virtual TFuture<void> InterruptJob(EInterruptReason reason) = 0;

    virtual TFuture<void> AbortJob(const TError& error) = 0;

    virtual TFuture<void> FailJob() = 0;
};

DEFINE_REFCOUNTED_TYPE(IJobHost)

////////////////////////////////////////////////////////////////////////////////

void BuildInitializingOperationAttributes(TOperationPtr operation, NYson::IYsonConsumer* consumer);
void BuildRunningOperationAttributes(TOperationPtr operation, NYson::IYsonConsumer* consumer);
void BuildExecNodeAttributes(TExecNodePtr node, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

EAbortReason GetAbortReason(const NJobTrackerClient::NProto::TJobResult& result);

////////////////////////////////////////////////////////////////////

Stroka MakeOperationCodicilString(const TOperationId& operationId);
TCodicilGuard MakeOperationCodicilGuard(const TOperationId& operationId);

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

