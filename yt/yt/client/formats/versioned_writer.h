#pragma once

#include "public.h"

#include <yt/client/table_client/versioned_writer.h>
#include <yt/client/table_client/schema.h>

#include <yt/core/actions/future.h>

#include <yt/core/concurrency/public.h>

#include <yt/core/misc/blob_output.h>

#include <yt/core/yson/writer.h>

namespace NYT::NFormats {

////////////////////////////////////////////////////////////////////////////////

class TVersionedWriter
    : public NTableClient::IVersionedWriter
{
public:
    TVersionedWriter(
        NConcurrency::IAsyncOutputStreamPtr stream,
        NTableClient::TTableSchemaPtr schema,
        const std::function<std::unique_ptr<NYson::IFlushableYsonConsumer>(IOutputStream*)>& consumerBuilder);

    virtual TFuture<void> Close() override;

    virtual bool Write(TRange<NTableClient::TVersionedRow> rows) override;

    virtual TFuture<void> GetReadyEvent() override;

private:
    const NConcurrency::IAsyncOutputStreamPtr Stream_;
    const NTableClient::TTableSchemaPtr Schema_;
    const std::unique_ptr<NYson::IFlushableYsonConsumer> Consumer_;

    TBlobOutput Buffer_;
    TFuture<void> Result_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFormats

