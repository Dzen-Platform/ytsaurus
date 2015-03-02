#pragma once

#include "public.h"

#include <core/ytree/yson_serializable.h>

#include <core/rpc/retrying_channel.h>

#include <ytlib/hydra/config.h>

#include <ytlib/transaction_client/config.h>

#include <ytlib/table_client/config.h>

#include <ytlib/new_table_client/config.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/hive/config.h>

#include <ytlib/api/config.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TDriverConfig
    : public NApi::TConnectionConfig
{
public:
    NApi::TFileReaderConfigPtr FileReader;
    NApi::TFileWriterConfigPtr FileWriter;
    NVersionedTableClient::TTableReaderConfigPtr TableReader;
    NVersionedTableClient::TTableWriterConfigPtr TableWriter;
    NApi::TJournalReaderConfigPtr JournalReader;
    NApi::TJournalWriterConfigPtr JournalWriter;
    i64 ReadBufferRowCount;
    i64 ReadBufferSize;

    i64 WriteBufferSize;
    int LightPoolSize;
    int HeavyPoolSize;

    TDriverConfig()
    {
        RegisterParameter("file_reader", FileReader)
            .DefaultNew();
        RegisterParameter("file_writer", FileWriter)
            .DefaultNew();
        RegisterParameter("table_reader", TableReader)
            .DefaultNew();
        RegisterParameter("table_writer", TableWriter)
            .DefaultNew();
        RegisterParameter("journal_reader", JournalReader)
            .DefaultNew();
        RegisterParameter("journal_writer", JournalWriter)
            .DefaultNew();

        RegisterParameter("read_buffer_row_count", ReadBufferRowCount)
            .Default((i64) 10000);
        RegisterParameter("read_buffer_size", ReadBufferSize)
            .Default((i64) 1 * 1024 * 1024);
        RegisterParameter("write_buffer_size", WriteBufferSize)
            .Default((i64) 1 * 1024 * 1024);
        RegisterParameter("light_pool_size", LightPoolSize)
            .Describe("Number of threads handling light requests")
            .Default(1);
        RegisterParameter("heavy_pool_size", HeavyPoolSize)
            .Describe("Number of threads handling heavy requests")
            .Default(4);
    }
};

DEFINE_REFCOUNTED_TYPE(TDriverConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

