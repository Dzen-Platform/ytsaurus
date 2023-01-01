#include "job_reporter.h"

#include "config.h"
#include "job_report.h"

#include <yt/yt/server/lib/misc/archive_reporter.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/controller_agent/helpers.h>

#include <yt/yt/ytlib/scheduler/helpers.h>
#include <yt/yt/ytlib/scheduler/records/job_fail_context.record.h>
#include <yt/yt/ytlib/scheduler/records/operation_id.record.h>
#include <yt/yt/ytlib/scheduler/records/job_profile.record.h>

#include <yt/yt/client/api/connection.h>
#include <yt/yt/client/api/transaction.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>

#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/record_helpers.h>

#include <yt/yt/core/compression/codec.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/delayed_executor.h>
#include <yt/yt/core/concurrency/nonblocking_batch.h>
#include <yt/yt/core/concurrency/async_semaphore.h>

#include <yt/yt/core/utilex/random.h>

namespace NYT::NJobAgent {

using namespace NNodeTrackerClient;
using namespace NTransactionClient;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;
using namespace NControllerAgent;
using namespace NApi;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NProfiling;
using namespace NScheduler;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

namespace {

static const TProfiler ReporterProfiler("/job_reporter");

////////////////////////////////////////////////////////////////////////////////

bool IsSpecEntry(const TJobReport& stat)
{
    return stat.Spec().operator bool();
}

////////////////////////////////////////////////////////////////////////////////

class TJobRowlet
    : public IArchiveRowlet
{
public:
    TJobRowlet(
        TJobReport&& report,
        bool reportStatisticsLz4,
        const std::optional<TString>& localAddress)
        : Report_(std::move(report))
        , ReportStatisticsLz4_(reportStatisticsLz4)
        , DefaultLocalAddress_(localAddress)
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobTableDescriptor::Get().Index;

        TYsonString coreInfosYsonString;
        TString jobCompetitionIdString;
        TString probingJobCompetitionIdString;
        TString statisticsLz4;
        TYsonString briefStatisticsYsonString;

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Report_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Report_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[1], index.JobIdLo));
        if (Report_.Type()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.Type(), index.Type));
        }
        if (Report_.State()) {
            builder.AddValue(MakeUnversionedStringValue(
                *Report_.State(),
                index.TransientState));
        }
        if (Report_.StartTime()) {
            builder.AddValue(MakeUnversionedInt64Value(*Report_.StartTime(), index.StartTime));
        }
        if (Report_.FinishTime()) {
            builder.AddValue(MakeUnversionedInt64Value(*Report_.FinishTime(), index.FinishTime));
        }
        if (DefaultLocalAddress_) {
            builder.AddValue(MakeUnversionedStringValue(*DefaultLocalAddress_, index.Address));
        }
        if (Report_.Error()) {
            builder.AddValue(MakeUnversionedAnyValue(*Report_.Error(), index.Error));
        }
        if (Report_.Statistics()) {
            if (ReportStatisticsLz4_) {
                auto codec = NCompression::GetCodec(NCompression::ECodec::Lz4);
                statisticsLz4 = ToString(codec->Compress(TSharedRef::FromString(*Report_.Statistics())));
                builder.AddValue(MakeUnversionedStringValue(statisticsLz4, index.StatisticsLz4));
            } else {
                builder.AddValue(MakeUnversionedAnyValue(*Report_.Statistics(), index.Statistics));
            }
            briefStatisticsYsonString = BuildBriefStatistics(ConvertToNode(TYsonStringBuf(*Report_.Statistics())));
            builder.AddValue(MakeUnversionedAnyValue(briefStatisticsYsonString.AsStringBuf(), index.BriefStatistics));
        }
        if (Report_.Events()) {
            builder.AddValue(MakeUnversionedAnyValue(*Report_.Events(), index.Events));
        }
        if (Report_.StderrSize()) {
            builder.AddValue(MakeUnversionedUint64Value(*Report_.StderrSize(), index.StderrSize));
        }
        if (Report_.CoreInfos()) {
            coreInfosYsonString = ConvertToYsonString(*Report_.CoreInfos());
            builder.AddValue(MakeUnversionedAnyValue(coreInfosYsonString.AsStringBuf(), index.CoreInfos));
        }
        builder.AddValue(MakeUnversionedInt64Value(TInstant::Now().MicroSeconds(), index.UpdateTime));
        if (Report_.Spec()) {
            builder.AddValue(MakeUnversionedBooleanValue(Report_.Spec().operator bool(), index.HasSpec));
        }
        if (Report_.FailContext()) {
            builder.AddValue(MakeUnversionedUint64Value(Report_.FailContext()->size(), index.FailContextSize));
        }
        if (Report_.JobCompetitionId()) {
            jobCompetitionIdString = ToString(Report_.JobCompetitionId());
            builder.AddValue(MakeUnversionedStringValue(jobCompetitionIdString, index.JobCompetitionId));
        }
        if (Report_.ProbingJobCompetitionId()) {
            probingJobCompetitionIdString = ToString(Report_.ProbingJobCompetitionId());
            builder.AddValue(MakeUnversionedStringValue(probingJobCompetitionIdString, index.ProbingJobCompetitionId));
        }
        if (Report_.HasCompetitors().has_value()) {
            builder.AddValue(MakeUnversionedBooleanValue(Report_.HasCompetitors().value(), index.HasCompetitors));
        }
        if (Report_.HasProbingCompetitors().has_value()) {
            builder.AddValue(MakeUnversionedBooleanValue(Report_.HasProbingCompetitors().value(), index.HasProbingCompetitors));
        }
        if (Report_.ExecAttributes()) {
            builder.AddValue(MakeUnversionedAnyValue(*Report_.ExecAttributes(), index.ExecAttributes));
        }
        if (Report_.TaskName()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.TaskName(), index.TaskName));
        }
        if (Report_.TreeId()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.TreeId(), index.PoolTree));
        }
        // COMPAT(levysotsky)
        if (archiveVersion >= 39 && Report_.MonitoringDescriptor()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.MonitoringDescriptor(), index.MonitoringDescriptor));
        }

        return builder.FinishRow();
    }

private:
    const TJobReport Report_;
    const bool ReportStatisticsLz4_;
    const std::optional<TString> DefaultLocalAddress_;
};

////////////////////////////////////////////////////////////////////////////////

class TOperationIdRowlet
    : public IArchiveRowlet
{
public:
    explicit TOperationIdRowlet(TJobReport&& report)
        : Report_(std::move(report))
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int /*archiveVersion*/) const override
    {
        NRecords::TOperationId record;
        record.JobIdHi = Report_.JobId().Parts64[0];
        record.JobIdLo = Report_.JobId().Parts64[1];
        record.OperationIdHi = Report_.OperationId().Parts64[0];
        record.OperationIdLo = Report_.OperationId().Parts64[1];
        return FromRecord(record);
    }

private:
    const TJobReport Report_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobSpecRowlet
    : public IArchiveRowlet
{
public:
    explicit TJobSpecRowlet(TJobReport&& report)
        : Report_(std::move(report))
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int /*archiveVersion*/) const override
    {
        const auto& index = TJobSpecTableDescriptor::Get().Index;

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[1], index.JobIdLo));
        if (Report_.Spec()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.Spec(), index.Spec));
        }
        if (Report_.SpecVersion()) {
            builder.AddValue(MakeUnversionedInt64Value(*Report_.SpecVersion(), index.SpecVersion));
        }
        if (Report_.Type()) {
            builder.AddValue(MakeUnversionedStringValue(*Report_.Type(), index.Type));
        }

        return builder.FinishRow();
    }

private:
    const TJobReport Report_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobStderrRowlet
    : public IArchiveRowlet
{
public:
    explicit TJobStderrRowlet(TJobReport&& report)
        : Report_(std::move(report))
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int /*archiveVersion*/) const override
    {
        const auto& index = TJobStderrTableDescriptor::Get().Index;

        if (!Report_.Stderr()) {
            return {};
        }

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Report_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Report_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Report_.JobId().Parts64[1], index.JobIdLo));
        builder.AddValue(MakeUnversionedStringValue(*Report_.Stderr(), index.Stderr));

        return builder.FinishRow();
    }

private:
    const TJobReport Report_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobFailContextRowlet
    : public IArchiveRowlet
{
public:
    explicit TJobFailContextRowlet(TJobReport&& report)
        : Report_(std::move(report))
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        if (archiveVersion < 21 || !Report_.FailContext() || Report_.FailContext()->size() > MaxStringValueLength) {
            return {};
        }

        NRecords::TJobFailContext record;
        record.OperationIdHi = Report_.OperationId().Parts64[0];
        record.OperationIdLo = Report_.OperationId().Parts64[1];
        record.JobIdHi = Report_.JobId().Parts64[0];
        record.JobIdLo = Report_.JobId().Parts64[1];
        record.FailContext = *Report_.FailContext();
        return FromRecord(record);
    }

private:
    const TJobReport Report_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobProfileRowlet
    : public IArchiveRowlet
{
public:
    explicit TJobProfileRowlet(TJobReport&& report)
        : Report_(std::move(report))
    { }

    size_t EstimateSize() const override
    {
        return Report_.EstimateSize();
    }

    TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& profile = Report_.Profile();

        if (archiveVersion < 27 || !profile) {
            return {};
        }

        NRecords::TJobProfile record;
        record.OperationIdHi = Report_.OperationId().Parts64[0];
        record.OperationIdLo = Report_.OperationId().Parts64[1];
        record.JobIdHi = Report_.JobId().Parts64[0];
        record.JobIdLo = Report_.JobId().Parts64[1];
        record.PartIndex = 0;
        record.ProfileType = profile->Type;
        record.ProfileBlob = profile->Blob;
        record.ProfilingProbability = profile->ProfilingProbability;
        return FromRecord(record);
    }

private:
    const TJobReport Report_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TJobReporter::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TJobReporterConfigPtr reporterConfig,
        const NApi::NNative::IConnectionPtr& connection,
        std::optional<TString> localAddress)
        : Client_(connection->CreateNativeClient(
            TClientOptions::FromUser(reporterConfig->User)))
        , Config_(std::move(reporterConfig))
        , LocalAddress_(std::move(localAddress))
        , JobHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->JobHandler,
                TJobTableDescriptor::Get().NameTable,
                "jobs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "jobs")))
        , OperationIdHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->OperationIdHandler,
                NRecords::TOperationIdDescriptor::Get()->GetNameTable(),
                "operation_ids",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "operation_ids")))
        , JobSpecHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->JobSpecHandler,
                TJobSpecTableDescriptor::Get().NameTable,
                "job_specs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "job_specs")))
        , JobStderrHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->JobStderrHandler,
                TJobStderrTableDescriptor::Get().NameTable,
                "stderrs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "stderrs")))
        , JobFailContextHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->JobFailContextHandler,
                NRecords::TJobFailContextDescriptor::Get()->GetNameTable(),
                "fail_contexts",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "fail_contexts")))
        , JobProfileHandler_(
            CreateArchiveReporter(
                Version_,
                Config_,
                Config_->JobProfileHandler,
                NRecords::TJobProfileDescriptor::Get()->GetNameTable(),
                "profiles",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "profiles")))
    { }

    void HandleJobReport(TJobReport&& jobReport)
    {
        if (IsSpecEntry(jobReport)) {
            JobSpecHandler_->Enqueue(std::make_unique<TJobSpecRowlet>(jobReport.ExtractSpec()));
        }
        if (jobReport.Stderr()) {
            JobStderrHandler_->Enqueue(std::make_unique<TJobStderrRowlet>(jobReport.ExtractStderr()));
        }
        if (jobReport.FailContext()) {
            JobFailContextHandler_->Enqueue(std::make_unique<TJobFailContextRowlet>(jobReport.ExtractFailContext()));
        }
        if (jobReport.Profile()) {
            JobProfileHandler_->Enqueue(std::make_unique<TJobProfileRowlet>(jobReport.ExtractProfile()));
        }
        if (!jobReport.IsEmpty()) {
            OperationIdHandler_->Enqueue(std::make_unique<TOperationIdRowlet>(jobReport.ExtractIds()));
            JobHandler_->Enqueue(std::make_unique<TJobRowlet>(
                std::move(jobReport),
                Config_->ReportStatisticsLz4,
                LocalAddress_));
        }
    }

    void SetEnabled(bool enable)
    {
        JobHandler_->SetEnabled(enable);
        OperationIdHandler_->SetEnabled(enable);
    }

    void SetOperationArchiveVersion(int version)
    {
        Version_->Set(version);
    }

    int ExtractWriteFailuresCount()
    {
        return
            JobHandler_->ExtractWriteFailuresCount() +
            JobSpecHandler_->ExtractWriteFailuresCount() +
            JobStderrHandler_->ExtractWriteFailuresCount() +
            JobFailContextHandler_->ExtractWriteFailuresCount() +
            JobProfileHandler_->ExtractWriteFailuresCount();
    }

    bool IsQueueTooLarge()
    {
        return
            JobHandler_->IsQueueTooLarge() ||
            JobSpecHandler_->IsQueueTooLarge() ||
            JobStderrHandler_->IsQueueTooLarge() ||
            JobFailContextHandler_->IsQueueTooLarge() ||
            JobProfileHandler_->IsQueueTooLarge();
    }

    void UpdateConfig(const TJobReporterConfigPtr& config)
    {
        JobHandler_->SetEnabled(config->EnableJobReporter);
        JobSpecHandler_->SetEnabled(config->EnableJobSpecReporter);
        JobStderrHandler_->SetEnabled(config->EnableJobStderrReporter);
        JobProfileHandler_->SetEnabled(config->EnableJobProfileReporter);
        JobFailContextHandler_->SetEnabled(config->EnableJobFailContextReporter);
    }

    void OnDynamicConfigChanged(
        const TJobReporterDynamicConfigPtr& /* oldConfig */,
        const TJobReporterDynamicConfigPtr& newConfig)
    {
        JobHandler_->SetEnabled(newConfig->EnableJobReporter.value_or(Config_->EnableJobReporter));
        JobSpecHandler_->SetEnabled(newConfig->EnableJobSpecReporter.value_or(Config_->EnableJobSpecReporter));
        JobStderrHandler_->SetEnabled(newConfig->EnableJobStderrReporter.value_or(Config_->EnableJobStderrReporter));
        JobProfileHandler_->SetEnabled(newConfig->EnableJobProfileReporter.value_or(Config_->EnableJobProfileReporter));
        JobFailContextHandler_->SetEnabled(newConfig->EnableJobFailContextReporter.value_or(Config_->EnableJobFailContextReporter));
    }

private:
    const NNative::IClientPtr Client_;
    const TJobReporterConfigPtr Config_;
    const std::optional<TString> LocalAddress_;
    const TActionQueuePtr Reporter_ = New<TActionQueue>("JobReporter");
    const TArchiveVersionHolderPtr Version_ = New<TArchiveVersionHolder>();
    const IArchiveReporterPtr JobHandler_;
    const IArchiveReporterPtr OperationIdHandler_;
    const IArchiveReporterPtr JobSpecHandler_;
    const IArchiveReporterPtr JobStderrHandler_;
    const IArchiveReporterPtr JobFailContextHandler_;
    const IArchiveReporterPtr JobProfileHandler_;
};

////////////////////////////////////////////////////////////////////////////////

TJobReporter::TJobReporter(
    TJobReporterConfigPtr reporterConfig,
    const NApi::NNative::IConnectionPtr& connection,
    std::optional<TString> localAddress)
    : Impl_(
        reporterConfig->Enabled
            ? New<TImpl>(std::move(reporterConfig), connection, std::move(localAddress))
            : nullptr)
{ }

TJobReporter::~TJobReporter() = default;

void TJobReporter::HandleJobReport(TJobReport&& jobReport)
{
    if (Impl_) {
        Impl_->HandleJobReport(std::move(jobReport));
    }
}

void TJobReporter::SetOperationArchiveVersion(int version)
{
    if (Impl_) {
        Impl_->SetOperationArchiveVersion(version);
    }
}

int TJobReporter::ExtractWriteFailuresCount()
{
    if (Impl_) {
        return Impl_->ExtractWriteFailuresCount();
    }
    return 0;
}

bool TJobReporter::GetQueueIsTooLarge()
{
    if (Impl_) {
        return Impl_->IsQueueTooLarge();
    }
    return false;
}

void TJobReporter::UpdateConfig(const TJobReporterConfigPtr& config)
{
    if (Impl_) {
        Impl_->UpdateConfig(config);
    }
}

void TJobReporter::OnDynamicConfigChanged(
    const TJobReporterDynamicConfigPtr& oldConfig,
    const TJobReporterDynamicConfigPtr& newConfig)
{
    if (Impl_) {
        Impl_->OnDynamicConfigChanged(oldConfig, newConfig);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
