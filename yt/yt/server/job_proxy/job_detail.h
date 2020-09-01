#pragma once

#include "public.h"
#include "job.h"

#include <yt/ytlib/chunk_client/public.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>

#include <yt/ytlib/job_tracker_client/proto/job.pb.h>

#include <yt/ytlib/scheduler/proto/job.pb.h>

#include <yt/client/table_client/schemaful_reader_adapter.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

void RunQuery(
    const NScheduler::NProto::TQuerySpec& querySpec,
    const NTableClient::TSchemalessReaderFactory& readerFactory,
    const NTableClient::TSchemalessWriterFactory& writerFactory);

////////////////////////////////////////////////////////////////////////////////

//! Base class for all jobs inside job proxy.
class TJob
    : public IJob
{
public:
    explicit TJob(IJobHost* host);

    virtual std::vector<NChunkClient::TChunkId> DumpInputContext() override;
    virtual TString GetStderr() override;
    virtual std::optional<TString> GetFailContext() override;
    virtual std::optional<NJobAgent::TJobProfile> GetProfile() override;
    virtual const NCoreDump::TCoreInfos& GetCoreInfos() const override;
    virtual NYson::TYsonString PollJobShell(const NYson::TYsonString& parameters) override;
    virtual void Fail() override;
    virtual TCpuStatistics GetCpuStatistics() const override;
    virtual i64 GetStderrSize() const override;
 
protected:
    IJobHost* Host_;
    const TInstant StartTime_;

    NChunkClient::TClientBlockReadOptions BlockReadOptions_;
};

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobBase
    : public TJob
{
public:
    explicit TSimpleJobBase(IJobHost* host);

    virtual void Initialize() override;

    virtual NJobTrackerClient::NProto::TJobResult Run() override;

    virtual void Cleanup() override;

    virtual double GetProgress() const override;

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;
    virtual NChunkClient::TInterruptDescriptor GetInterruptDescriptor() const override;

    virtual TStatistics GetStatistics() const override;

    virtual bool ShouldSendBoundaryKeys() const;

    virtual void Interrupt() override;

protected:
    const NJobTrackerClient::NProto::TJobSpec& JobSpec_;
    const NScheduler::NProto::TSchedulerJobSpecExt& SchedulerJobSpecExt_;

    NChunkClient::IMultiReaderMemoryManagerPtr MultiReaderMemoryManager_;

    NTableClient::ISchemalessMultiChunkReaderPtr Reader_;
    NTableClient::ISchemalessMultiChunkWriterPtr Writer_;
    NTableClient::TSchemalessReaderFactory ReaderFactory_;
    NTableClient::TSchemalessWriterFactory WriterFactory_;

    i64 TotalRowCount_ = 0;

    std::atomic<bool> Initialized_ = {false};
    std::atomic<bool> Interrupted_ = {false};

    virtual void CreateReader() = 0;
    virtual void CreateWriter() = 0;

    virtual i64 GetTotalReaderMemoryLimit() const = 0;

    NTableClient::TTableWriterConfigPtr GetWriterConfig(const NScheduler::NProto::TTableOutputSpec& outputSpec);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
