#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/data_slice_descriptor.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/core/logging/log.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TUserJobWriteController
{
public:
    explicit TUserJobWriteController(IJobHost* host);
    ~TUserJobWriteController();

    void Init();

    std::vector<NTableClient::ISchemalessMultiChunkWriterPtr> GetWriters() const;
    IOutputStream* GetStderrTableWriter() const;

    void PopulateResult(NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt);
    void PopulateStderrResult(NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt);

protected:
    const IJobHost* Host_;
    const NLogging::TLogger Logger;

    std::atomic<bool> Initialized_ = {false};

    std::vector<NTableClient::ISchemalessMultiChunkWriterPtr> Writers_;
    std::unique_ptr<NTableClient::TBlobTableWriter> StderrTableWriter_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
