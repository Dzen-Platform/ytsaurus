#include "config.h"

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

void TBlobTableWriterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("max_part_size", &TThis::MaxPartSize)
        .Default(4_MB)
        .GreaterThanOrEqual(1_MB)
        .LessThanOrEqual(MaxRowWeightLimit);
}

////////////////////////////////////////////////////////////////////////////////

void TBufferedTableWriterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("retry_backoff_time", &TThis::RetryBackoffTime)
        .Default(TDuration::Seconds(3));
    registrar.Parameter("flush_period", &TThis::FlushPeriod)
        .Default(TDuration::Seconds(60));
    registrar.Parameter("row_buffer_chunk_size", &TThis::RowBufferChunkSize)
        .Default(64_KB);
}

////////////////////////////////////////////////////////////////////////////////

TTableColumnarStatisticsCacheConfig::TTableColumnarStatisticsCacheConfig()
{
    RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
        .Default(100'000);
    RegisterParameter("max_chunks_per_locate_request", MaxChunksPerLocateRequest)
        .Default(10'000);
    RegisterParameter("fetcher", Fetcher)
        .DefaultNew();
    RegisterParameter("columnar_statistics_fetcher_mode", ColumnarStatisticsFetcherMode)
        .Default(EColumnarStatisticsFetcherMode::Fallback);
}

////////////////////////////////////////////////////////////////////////////////

void THunkChunkPayloadWriterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("desired_block_size", &TThis::DesiredBlockSize)
        .GreaterThan(0)
        .Default(1_MBs);
}

///////////////////////////////////////////////////////////////////////////////

TBatchHunkReaderConfig::TBatchHunkReaderConfig()
{
    RegisterParameter("max_hunk_count_per_read", MaxHunkCountPerRead)
        .GreaterThan(0)
        .Default(10'000);
    RegisterParameter("max_total_hunk_length_per_read", MaxTotalHunkLengthPerRead)
        .GreaterThan(0)
        .Default(16_MB);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
