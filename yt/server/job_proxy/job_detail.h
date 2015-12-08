#pragma once

#include "public.h"
#include "job.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/job_tracker_client/job.pb.h>

#include <yt/ytlib/scheduler/job.pb.h>

#include <yt/ytlib/table_client/public.h>
#include <yt/ytlib/table_client/schemaful_reader_adapter.h>
#include <yt/ytlib/table_client/schemaful_writer_adapter.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

//! Base class for all jobs inside job proxy.
class TJob
    : public IJob
{
public:
    explicit TJob(IJobHost* host);

protected:
    TWeakPtr<IJobHost> Host;

    TInstant StartTime;

    virtual std::vector<NChunkClient::TChunkId> DumpInputContext() override;
    virtual NYson::TYsonString Strace() override;
    virtual void SignalJob(const Stroka& signalName) override;

};

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobBase
    : public TJob
{
public:
    explicit TSimpleJobBase(IJobHost *host);

    virtual NJobTrackerClient::NProto::TJobResult Run() override;

    virtual void Abort() override;

    virtual double GetProgress() const override;

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;

    virtual NJobTrackerClient::TStatistics GetStatistics() const override;

protected:
    const NJobTrackerClient::NProto::TJobSpec& JobSpec_;
    const NScheduler::NProto::TSchedulerJobSpecExt& SchedulerJobSpecExt_;

    NTableClient::ISchemalessMultiChunkReaderPtr Reader_;
    NTableClient::ISchemalessMultiChunkWriterPtr Writer_;
    NTableClient::TSchemalessReaderFactory ReaderFactory_;
    NTableClient::TSchemalessWriterFactory WriterFactory_;

    i64 TotalRowCount_;

    virtual void CreateReader() = 0;
    virtual void CreateWriter() = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
