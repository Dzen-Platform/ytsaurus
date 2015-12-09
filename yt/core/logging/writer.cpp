#include "writer.h"
#include "private.h"
#include "log.h"

#include <yt/build/build.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/fs.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

static const TLogger Logger(SystemLoggingCategory);
static const size_t BufferSize = 1 << 16;

////////////////////////////////////////////////////////////////////////////////

namespace {

TLogEvent GetBannerEvent()
{
    TLogEvent event;
    event.DateTime = TInstant::Now();
    event.Category = SystemLoggingCategory;
    event.Level = ELogLevel::Info;
    event.Message = Format("Logging started (Version: %v, BuildHost: %v, BuildTime: %v)",
        GetVersion(),
        GetBuildHost(),
        GetBuildTime());
    return event;
}

} // namespace

class TStreamLogWriterBase::TCachingDateFormatter
{
public:
    TCachingDateFormatter()
    {
        Update(TInstant::Now());
    }

    void Format(TMessageBuffer* out, TInstant dateTime)
    {
        if (dateTime.MicroSeconds() >= Deadline_) {
            Update(dateTime);
        }
        out->AppendString(Cached_.GetData());
    }

private:
    void Update(TInstant dateTime)
    {
        Cached_.Reset();
        FormatDateTime(&Cached_, dateTime);
        Cached_.AppendChar('\0');
        Deadline_ = dateTime.MicroSeconds() + 1000 - (dateTime.MicroSeconds() % 1000);
    }

    TMessageBuffer Cached_;
    ui64 Deadline_;
};

TStreamLogWriterBase::TStreamLogWriterBase()
    : Buffer_(new TMessageBuffer())
    , CachingDateFormatter_(new TCachingDateFormatter())
{ }

TStreamLogWriterBase::~TStreamLogWriterBase() = default;

void TStreamLogWriterBase::Write(const TLogEvent& event)
{
    auto* stream = GetOutputStream();
    if (!stream) {
        return;
    }

    auto* buffer = Buffer_.get();
    buffer->Reset();

    CachingDateFormatter_->Format(buffer, event.DateTime);
    buffer->AppendChar('\t');

    FormatLevel(buffer, event.Level);
    buffer->AppendChar('\t');

    buffer->AppendString(event.Category);
    buffer->AppendChar('\t');

    FormatMessage(buffer, event.Message);
    buffer->AppendChar('\t');

    if (event.ThreadId != NConcurrency::InvalidThreadId) {
        buffer->AppendNumber(event.ThreadId, 16);
    }
    buffer->AppendChar('\t');

    if (event.FiberId != NConcurrency::InvalidFiberId) {
        buffer->AppendNumber(event.FiberId, 16);
    }
    buffer->AppendChar('\t');

    if (event.TraceId != NTracing::InvalidTraceId) {
        buffer->AppendNumber(event.TraceId, 16);
    }
    buffer->AppendChar('\n');

    try {
        stream->Write(buffer->GetData(), buffer->GetBytesWritten());
    } catch (const std::exception& ex) {
        OnException(ex);
    }
}

void TStreamLogWriterBase::Flush()
{
    auto* stream = GetOutputStream();
    if (!stream) {
        return;
    }

    try {
        stream->Flush();
    } catch (const std::exception& ex) {
        OnException(ex);
    }
}

void TStreamLogWriterBase::Reload()
{ }

void TStreamLogWriterBase::CheckSpace(i64 minSpace)
{ }

void TStreamLogWriterBase::OnException(const std::exception& ex)
{
    // Fail with drama by default.
    TRawFormatter<1024> formatter;
    formatter.AppendString("\n*** Unhandled exception in log writer: ");
    formatter.AppendString(ex.what());
    formatter.AppendString("\n*** Aborting ***\n");

    auto unused = ::write(2, formatter.GetData(), formatter.GetBytesWritten());
    (void)unused;

    std::terminate();
}

////////////////////////////////////////////////////////////////////////////////

TOutputStream* TStreamLogWriter::GetOutputStream() const noexcept
{
    return Stream_;
}

////////////////////////////////////////////////////////////////////////////////

TOutputStream* TStderrLogWriter::GetOutputStream() const noexcept
{
    return &Cerr;
}

////////////////////////////////////////////////////////////////////////////////

TOutputStream* TStdoutLogWriter::GetOutputStream() const noexcept
{
    return &Cout;
}

////////////////////////////////////////////////////////////////////////////////

TFileLogWriter::TFileLogWriter(const Stroka& fileName)
    : FileName_(fileName)
{
    Open();
}

TFileLogWriter::~TFileLogWriter() = default;

TOutputStream* TFileLogWriter::GetOutputStream() const noexcept
{
    if (Y_LIKELY(!Disabled_.load(std::memory_order_acquire))) {
        return FileOutput_.get();
    } else {
        return nullptr;
    }
}

void TFileLogWriter::OnException(const std::exception& ex)
{
    Disabled_ = true;
    LOG_ERROR(ex, "Disabled log file (FileName: %v)", FileName_);

    Close();
}

void TFileLogWriter::CheckSpace(i64 minSpace)
{
    try {
        auto directoryName = NFS::GetDirectoryName(FileName_);
        auto statistics = NFS::GetDiskSpaceStatistics(directoryName);
        if (statistics.AvailableSpace < minSpace) {
            if (!Disabled_.load(std::memory_order_acquire)) {
                Disabled_ = true;
                LOG_ERROR("Log file disabled: not enough space available (FileName: %v, AvailableSpace: %v, MinSpace: %v)",
                    directoryName,
                    statistics.AvailableSpace,
                    minSpace);

                Close();
            }
        } else {
            if (Disabled_.load(std::memory_order_acquire)) {
                Reload(); // Reinitialize all descriptors.

                LOG_INFO("Log file enabled: space check passed (FileName: %v)", FileName_);
                Disabled_ = false;
            }
        }
    } catch (const std::exception& ex) {
        Disabled_ = true;
        LOG_ERROR(ex, "Log file disabled: space check failed (FileName: %v)", FileName_);

        Close();
    }
}

void TFileLogWriter::Open()
{
    try {
        NFS::ForcePath(NFS::GetDirectoryName(FileName_));
        File_.reset(new TFile(FileName_, OpenAlways|ForAppend|WrOnly|Seq|CloseOnExec));
        FileOutput_.reset(new TBufferedFileOutput(*File_, BufferSize));
        FileOutput_->SetFinishPropagateMode(true);

        // Emit a delimiter for ease of navigation.
        if (File_->GetLength() > 0) {
            *FileOutput_ << Endl;
        }

        Write(GetBannerEvent());
    } catch (const std::exception& ex) {
        Disabled_ = true;
        LOG_ERROR(ex, "Failed to open log file (FileName: %v)", FileName_);

        Close();
    } catch (...) {
        YUNREACHABLE();
    }
}

void TFileLogWriter::Close()
{
    try {
        if (FileOutput_) {
            FileOutput_->Flush();
            FileOutput_->Finish();
        }
        if (File_) {
            File_->Close();
        }
    } catch (const std::exception& ex) {
        Disabled_ = true;
        LOG_ERROR(ex, "Failed to close log file %v", FileName_);
    } catch (...) {
        YUNREACHABLE();
    }

    try {
        FileOutput_.reset();
        File_.reset();
    } catch (...) {
        YUNREACHABLE();
    }
}

void TFileLogWriter::Reload()
{
    Close();
    Open();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
