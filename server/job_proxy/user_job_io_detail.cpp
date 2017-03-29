#include "user_job_io_detail.h"
#include "config.h"
#include "job.h"

#include <yt/server/misc/job_table_schema.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/job_proxy/user_job_io_factory.h>

#include <yt/ytlib/table_client/blob_table_writer.h>
#include <yt/ytlib/table_client/helpers.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/misc/finally.h>

namespace NYT {
namespace NJobProxy {

using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NObjectClient;
using namespace NYTree;

using NChunkClient::TDataSliceDescriptor;

////////////////////////////////////////////////////////////////////////////////

TUserJobIOBase::TUserJobIOBase(IJobHostPtr host)
    : Host_(host)
    , Logger(host->GetLogger())
{ }

TUserJobIOBase::~TUserJobIOBase()
{ }

void TUserJobIOBase::Init()
{
    LOG_INFO("Opening writers");

    auto guard = Finally([&] () {
        Initialized_ = true;
    });

    auto userJobIOFactory = CreateUserJobIOFactory(Host_->GetJobSpecHelper());

    const auto& schedulerJobSpecExt = Host_->GetJobSpecHelper()->GetSchedulerJobSpecExt();
    auto transactionId = FromProto<TTransactionId>(schedulerJobSpecExt.output_transaction_id());
    for (const auto& outputSpec : schedulerJobSpecExt.output_table_specs()) {
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));
        options->EnableValidationOptions();

        auto writerConfig = Host_->GetJobSpecHelper()->GetJobIOConfig()->TableWriter;
        if (outputSpec.has_table_writer_config()) {
            writerConfig = UpdateYsonSerializable(
                writerConfig,
                ConvertTo<INodePtr>(TYsonString(outputSpec.table_writer_config())));
        }

        auto timestamp = static_cast<TTimestamp>(outputSpec.timestamp());
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());

        TTableSchema schema;
        if (outputSpec.has_table_schema()) {
            schema = FromProto<TTableSchema>(outputSpec.table_schema());
        }

        auto writer = userJobIOFactory->CreateWriter(
            Host_->GetClient(),
            writerConfig,
            options,
            chunkListId,
            transactionId,
            schema,
            TChunkTimestamps{timestamp, timestamp});

        // ToDo(psushin): open writers in parallel.
        auto error = WaitFor(writer->Open());
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
        Writers_.push_back(writer);
    }

    if (schedulerJobSpecExt.user_job_spec().has_stderr_table_spec()) {
        const auto& stderrTableSpec = schedulerJobSpecExt.user_job_spec().stderr_table_spec();
        const auto& outputTableSpec = stderrTableSpec.output_table_spec();
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(stderrTableSpec.output_table_spec().table_writer_options()));
        options->EnableValidationOptions();

        auto stderrTableWriterConfig = ConvertTo<TBlobTableWriterConfigPtr>(
            TYsonString(stderrTableSpec.blob_table_writer_config()));

        StderrTableWriter_.reset(
            new NTableClient::TBlobTableWriter(
                GetStderrBlobTableSchema(),
                {ConvertToYsonString(Host_->GetJobId())},
                Host_->GetClient(),
                stderrTableWriterConfig,
                options,
                transactionId,
                FromProto<TChunkListId>(outputTableSpec.chunk_list_id())));
    }
}

std::vector<ISchemalessMultiChunkWriterPtr> TUserJobIOBase::GetWriters() const
{
    if (Initialized_) {
        return Writers_;
    } else {
        return std::vector<ISchemalessMultiChunkWriterPtr>();
    }
}

TOutputStream* TUserJobIOBase::GetStderrTableWriter() const
{
    if (Initialized_) {
        return StderrTableWriter_.get();
    } else {
        return nullptr;
    }
}

void TUserJobIOBase::PopulateResult(TSchedulerJobResultExt* schedulerJobResultExt)
{
    for (const auto& writer : Writers_) {
        *schedulerJobResultExt->add_output_boundary_keys() = GetWrittenChunksBoundaryKeys(writer);
    }
}

void TUserJobIOBase::PopulateStderrResult(NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt)
{
    if (StderrTableWriter_) {
        *schedulerJobResultExt->mutable_stderr_table_boundary_keys() = StderrTableWriter_->GetOutputResult();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
