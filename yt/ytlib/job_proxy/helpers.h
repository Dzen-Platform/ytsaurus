#pragma once

#include <yt/ytlib/chunk_client/data_slice_descriptor.h>

#include <yt/client/table_client/schemaful_reader_adapter.h>

#include <yt/ytlib/scheduler/proto/job.pb.h>
#include <yt/ytlib/table_client/partitioner.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

void RunQuery(
    const NScheduler::NProto::TQuerySpec& querySpec,
    const NTableClient::TSchemalessReaderFactory& readerFactory,
    const NTableClient::TSchemalessWriterFactory& writerFactory,
    const std::optional<TString>& udfDirectory);

std::vector<NChunkClient::TDataSliceDescriptor> UnpackDataSliceDescriptors(const NScheduler::NProto::TTableInputSpec& inputTableSpec);

NTableClient::IPartitionerPtr CreatePartitioner(const NScheduler::NProto::TPartitionJobSpecExt& partitionJobSpecExt);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
