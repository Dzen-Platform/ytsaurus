#pragma once

#include <yt/yt/ytlib/job_tracker_client/public.h>

#include <yt/yt/ytlib/job_tracker_client/proto/job_tracker_service.pb.h>

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/core/misc/public.h>

namespace NYT::NJobTrackerClient {

////////////////////////////////////////////////////////////////////////////////

TString JobTypeAsKey(EJobType jobType);

////////////////////////////////////////////////////////////////////////////////

struct TReleaseJobFlags
{
    bool ArchiveJobSpec = false;
    bool ArchiveStderr = false;
    bool ArchiveFailContext = false;
    bool ArchiveProfile = false;

    bool IsNonTrivial() const;

    void Persist(const TStreamPersistenceContext& context);
};

struct TJobToRelease
{
    TJobId JobId;
    TReleaseJobFlags ReleaseFlags = {};
};

TString ToString(const TReleaseJobFlags& releaseFlags);

////////////////////////////////////////////////////////////////////////////////

NTableClient::TTableSchemaPtr RenameColumnsInSchema(
    TStringBuf name,
    const NTableClient::TTableSchemaPtr& schema,
    bool isDynamic,
    const NTableClient::TColumnRenameDescriptors& renameDescriptors,
    bool changeStableName);

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

void ToProto(
    NProto::TJobToRemove* protoJobToRemove,
    const NJobTrackerClient::TJobToRelease& jobToRelease);

void FromProto(
    NJobTrackerClient::TJobToRelease* jobToRelease,
    const NProto::TJobToRemove& protoJobToRemove);

void ToProto(
    NProto::TReleaseJobFlags* protoReleaseJobFlags,
    const NJobTrackerClient::TReleaseJobFlags& releaseJobFlags);

void FromProto(
    NJobTrackerClient::TReleaseJobFlags* releaseJobFlags,
    const NProto::TReleaseJobFlags& protoReleaseJobFlags);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobTrackerClient

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
