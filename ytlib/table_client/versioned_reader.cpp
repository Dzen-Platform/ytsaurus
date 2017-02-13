#include "versioned_reader.h"

#include <yt/ytlib/chunk_client/data_statistics.pb.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

class TEmptyVersionedReader
    : public IVersionedReader
{
public:
    explicit TEmptyVersionedReader(int rowCount)
        : RowCount_(rowCount)
    { }

    virtual TFuture<void> Open() override
    {
        return VoidFuture;
    }

    virtual bool Read(std::vector<TVersionedRow>* rows) override
    {
        rows->clear();

        if (RowCount_ == 0) {
            return false;
        }

        int count = std::min(static_cast<int>(rows->capacity()), RowCount_);
        for (int index = 0; index < count; ++index) {
            rows->push_back(TVersionedRow());
        }

        RowCount_ -= count;
        return true;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return VoidFuture;
    }

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override
    {
        Y_UNREACHABLE();
    }

    virtual bool IsFetchingCompleted() const override
    {
        Y_UNREACHABLE();
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        Y_UNREACHABLE();
    }

private:
    int RowCount_;
};

DEFINE_REFCOUNTED_TYPE(TEmptyVersionedReader)

////////////////////////////////////////////////////////////////////////////////

IVersionedReaderPtr CreateEmptyVersionedReader(int rowCount)
{
    return New<TEmptyVersionedReader>(rowCount);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
