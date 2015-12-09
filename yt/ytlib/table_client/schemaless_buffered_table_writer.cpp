#include "schemaless_buffered_table_writer.h"
#include "private.h"
#include "config.h"
#include "name_table.h"
#include "row_buffer.h"
#include "schemaless_chunk_writer.h"

#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/ytlib/transaction_client/transaction_manager.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/chunked_memory_pool.h>
#include <yt/core/misc/common.h>
#include <yt/core/misc/property.h>

#include <yt/core/rpc/channel.h>

#include <queue>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NRpc;
using namespace NApi;
using namespace NTransactionClient;
using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

namespace {

class TBuffer
{
public:
    DEFINE_BYREF_RO_PROPERTY(std::vector<TUnversionedRow>, Rows);
    DEFINE_BYVAL_RW_PROPERTY(int, Index);

public:
    void Write(const std::vector<TUnversionedRow>& rows)
    {
        auto capturedRows = RowBuffer_->Capture(rows);
        Rows_.insert(Rows_.end(), capturedRows.begin(), capturedRows.end());
    }

    void Clear()
    {
        Rows_.clear();
        RowBuffer_->Clear();
    }

    i64 GetSize() const
    {
        return RowBuffer_->GetSize();
    }

    bool IsEmpty() const
    {
        return Rows_.empty();
    }

private:
    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>();

};

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TBufferedTableWriter
    : public ISchemalessWriter
{
public:
    TBufferedTableWriter(
        TBufferedTableWriterConfigPtr config,
        TRemoteWriterOptionsPtr options,
        IClientPtr client,
        TNameTablePtr nameTable,
        const TYPath& path)
        : Config_(config)
        , Options_(options)
        , Client_(client)
        , NameTable_(nameTable)
        , Path_(path)
        , FlushExecutor_(New<TPeriodicExecutor>(
            TDispatcher::Get()->GetWriterInvoker(),
            BIND(&TBufferedTableWriter::OnPeriodicFlush, Unretained(this)), Config_->FlushPeriod))
    {
        EmptyBuffers_.push(Buffers_);
        EmptyBuffers_.push(Buffers_ + 1);

        Logger.AddTag("Path: %v", Path_);
    }

    virtual TFuture<void> Open() override
    {
        FlushExecutor_->Start();
        return VoidFuture;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        YUNREACHABLE();
    }

    virtual TFuture<void> Close() override
    {
        YUNREACHABLE();
    }

    virtual bool Write(const std::vector<TUnversionedRow>& rows) override
    {
        TGuard<TSpinLock> guard(SpinLock_);

        if (!CurrentBuffer_) {
            if (EmptyBuffers_.empty()) {
                LOG_DEBUG("Buffer overflown; dropping rows");
                DroppedRowCount_ += rows.size();
                return true;
            }

            ++CurrentBufferIndex_;
            CurrentBuffer_ = EmptyBuffers_.front();
            EmptyBuffers_.pop();
            CurrentBuffer_->SetIndex(CurrentBufferIndex_);
        }

        CurrentBuffer_->Write(rows);

        if (CurrentBuffer_->GetSize() > Config_->DesiredChunkSize) {
            RotateBuffers();
        }

        return true;
    }

    virtual bool IsSorted() const override
    {
        return false;
    }

    virtual TNameTablePtr GetNameTable() const override
    {
        return NameTable_;
    }

private:
    const TBufferedTableWriterConfigPtr Config_;
    const TRemoteWriterOptionsPtr Options_;
    const IClientPtr Client_;
    const TNameTablePtr NameTable_;
    const TYPath Path_;

    const TPeriodicExecutorPtr FlushExecutor_;

    // Double buffering.
    TBuffer Buffers_[2];

    // Guards the following section of members.
    TSpinLock SpinLock_;
    int CurrentBufferIndex_ = -1;
    i64 DroppedRowCount_ = 0;
    TBuffer* CurrentBuffer_ = nullptr;
    std::queue<TBuffer*> EmptyBuffers_;

    // Only accessed in writer thread.
    int FlushedBufferCount_ = 0;

    NLogging::TLogger Logger = TableClientLogger;


    void OnPeriodicFlush()
    {
        TGuard<TSpinLock> guard(SpinLock_);

        if (CurrentBuffer_ && !CurrentBuffer_->IsEmpty()) {
            RotateBuffers();
        }
    }

    void RotateBuffers()
    {
        if (CurrentBuffer_) {
            ScheduleBufferFlush(CurrentBuffer_);
            CurrentBuffer_ = nullptr;
        }
    }

    void ScheduleBufferFlush(TBuffer* buffer)
    {
        LOG_DEBUG("Scheduling table chunk flush (BufferIndex: %v)",
            buffer->GetIndex());

        TDispatcher::Get()->GetWriterInvoker()->Invoke(BIND(
            &TBufferedTableWriter::FlushBuffer,
            MakeWeak(this),
            buffer));
    }

    void ScheduleDelayedRetry(TBuffer* buffer)
    {
        TDelayedExecutor::Submit(
            BIND(&TBufferedTableWriter::ScheduleBufferFlush, MakeWeak(this), buffer),
            Config_->RetryBackoffTime);
    }

    void FlushBuffer(TBuffer* buffer)
    {
        if (buffer->GetIndex() > FlushedBufferCount_) {
            // Previous chunk not yet flushed.
            ScheduleDelayedRetry(buffer);
            return;
        }

        try {
            TRichYPath richPath(Path_);
            richPath.Attributes().Set("append", true);

            auto writer = CreateSchemalessTableWriter(
                Config_,
                Options_,
                richPath,
                NameTable_,
                TKeyColumns(),
                Client_,
                nullptr);

            WaitFor(writer->Open())
                .ThrowOnError();
            writer->Write(buffer->Rows());
            WaitFor(writer->Close())
                .ThrowOnError();

            LOG_DEBUG("Buffered table chunk flushed successfully (BufferIndex: %v)",
                buffer->GetIndex());

            buffer->Clear();
            ++FlushedBufferCount_;

            {
                TGuard<TSpinLock> guard(SpinLock_);
                EmptyBuffers_.push(buffer);
            }
        } catch (const std::exception& ex) {
            LOG_WARNING(ex, "Buffered table chunk write failed, will retry later (BufferIndex: %v)",
                buffer->GetIndex());

            ScheduleDelayedRetry(buffer);
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

ISchemalessWriterPtr CreateSchemalessBufferedTableWriter(
    TBufferedTableWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    IClientPtr client,
    TNameTablePtr nameTable,
    const TYPath& path)
{
    return New<TBufferedTableWriter>(
        config,
        options,
        client,
        nameTable,
        path);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
