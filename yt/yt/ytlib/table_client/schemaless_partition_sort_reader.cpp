#include "schemaless_partition_sort_reader.h"
#include "private.h"
#include "config.h"
#include "partition_chunk_reader.h"
#include "schemaless_block_reader.h"
#include "timing_reader.h"

#include <yt/client/api/client.h>

#include <yt/client/table_client/unversioned_row_batch.h>

#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/heap.h>
#include <yt/core/misc/varint.h>

#include <yt/core/profiling/profiler.h>

#include <util/system/yield.h>

#include <util/random/shuffle.h>

namespace NYT::NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;

using NRpc::IChannelPtr;
using NNodeTrackerClient::TNodeDirectoryPtr;
using NChunkClient::TDataSliceDescriptor;
using NYT::TRange;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TableClientLogger;
static const auto& Profiler = TableClientProfiler;

static const int SortBucketSize = 10000;
static const int SpinsBetweenYield = 1000;
static const int RowsBetweenAtomicUpdate = 10000;
static const i32 BucketEndSentinel = -1;
static const double ReallocationFactor = 1.1;

////////////////////////////////////////////////////////////////////////////////

struct TSchemalessPartitionSortReaderTag
{ };

class TSchemalessPartitionSortReader
    : public ISchemalessMultiChunkReader
    , public TTimingReaderBase
{
public:
    TSchemalessPartitionSortReader(
        TMultiChunkReaderConfigPtr config,
        NApi::NNative::IClientPtr client,
        IBlockCachePtr blockCache,
        TNodeDirectoryPtr nodeDirectory,
        TKeyColumns keyColumns,
        TNameTablePtr nameTable,
        TClosure onNetworkReleased,
        const TDataSourceDirectoryPtr& dataSourceDirectory,
        std::vector<TDataSliceDescriptor> dataSliceDescriptors,
        int estimatedRowCount,
        bool approximate,
        int partitionTag,
        const TClientBlockReadOptions& blockReadOptions,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler,
        IMultiReaderMemoryManagerPtr multiReaderMemoryManager)
        : KeyColumns_(std::move(keyColumns))
        , KeyColumnCount_(static_cast<int>(KeyColumns_.size()))
        , OnNetworkReleased_(std::move(onNetworkReleased))
        , NameTable_(std::move(nameTable))
        , Approximate_(approximate)
        , EstimatedRowCount_(estimatedRowCount)
        , KeyBuffer_(this)
        , RowDescriptorBuffer_(this)
        , Buckets_(this)
        , BucketStart_(this)
        , SortComparer_(this)
        , MergeComparer_(this)
        , MemoryPool_(TSchemalessPartitionSortReaderTag())
    {
        YT_VERIFY(EstimatedRowCount_ <= std::numeric_limits<i32>::max());

        Shuffle(dataSliceDescriptors.begin(), dataSliceDescriptors.end());

        auto options = New<NTableClient::TTableReaderOptions>();
        options->KeepInMemory = true;

        UnderlyingReader_ = CreatePartitionMultiChunkReader(
            std::move(config),
            std::move(options),
            std::move(client),
            std::move(blockCache),
            std::move(nodeDirectory),
            std::move(dataSourceDirectory),
            std::move(dataSliceDescriptors),
            NameTable_,
            KeyColumns_,
            partitionTag,
            blockReadOptions,
            std::move(trafficMeter),
            std::move(bandwidthThrottler),
            std::move(rpsThrottler),
            std::move(multiReaderMemoryManager));

        SetReadyEvent(BIND(&TSchemalessPartitionSortReader::DoOpen, MakeWeak(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run());
    }

    virtual IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        MemoryPool_.Clear();

        if (!ReadyEvent().IsSet() || !ReadyEvent().Get().IsOK()) {
            return CreateEmptyUnversionedRowBatch();
        }

        if (ReadRowCount_ == TotalRowCount_) {
            SortQueue_->Shutdown();
            return nullptr;
        }

        bool mergeFinished = MergeFinished_.load();
        i64 sortedRowCount = SortedRowCount_.load();
        for (int spinCounter = 1; ; ++spinCounter) {
            if (sortedRowCount > ReadRowCount_ || mergeFinished) {
                break;
            }

            if (spinCounter % SpinsBetweenYield == 0) {
                ThreadYield();
            } else {
                SpinLockPause();
            }

            mergeFinished = MergeFinished_.load();
            sortedRowCount = SortedRowCount_.load();
        }

        if (mergeFinished && !MergeError_.IsOK()) {
            SetReadyEvent(MakeFuture(MergeError_));
            return CreateEmptyUnversionedRowBatch();
        }

        std::vector<TUnversionedRow> rows;
        rows.reserve(options.MaxRowsPerRead);
        i64 dataWeight = 0;

        while (ReadRowCount_ < sortedRowCount &&
               rows.size() < options.MaxRowsPerRead &&
               dataWeight < options.MaxDataWeightPerRead)
        {
            auto sortedIndex = SortedIndexes_[ReadRowCount_];
            auto& rowDescriptor = RowDescriptorBuffer_[sortedIndex];
            YT_VERIFY(rowDescriptor.BlockReader->JumpToRowIndex(rowDescriptor.RowIndex));
            auto row = rowDescriptor.BlockReader->GetRow(&MemoryPool_);
            rows.push_back(row);
            dataWeight += GetDataWeight(row);
            ++ReadRowCount_;
        }

        ReadDataWeight_ += dataWeight;

        YT_VERIFY(!rows.empty());
        return CreateBatchFromUnversionedRows(MakeSharedRange(std::move(rows), MakeStrong(this)));
    }

    virtual const TDataSliceDescriptor& GetCurrentReaderDescriptor() const override
    {
        YT_ABORT();
    }

    virtual i64 GetSessionRowIndex() const override
    {
        return ReadRowCount_;
    }

    virtual i64 GetTotalRowCount() const override
    {
        return TotalRowCount_;
    }

    virtual const TNameTablePtr& GetNameTable() const override
    {
        return NameTable_;
    }

    virtual const TKeyColumns& GetKeyColumns() const override
    {
        return KeyColumns_;
    }

    virtual bool IsFetchingCompleted() const override
    {
        YT_VERIFY(UnderlyingReader_);
        return UnderlyingReader_->IsFetchingCompleted();
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        YT_VERIFY(UnderlyingReader_);
        auto dataStatistics = UnderlyingReader_->GetDataStatistics();
        dataStatistics.set_row_count(ReadRowCount_);
        dataStatistics.set_data_weight(ReadDataWeight_);
        return dataStatistics;
    }

    virtual NChunkClient::TCodecStatistics GetDecompressionStatistics() const override
    {
        YT_VERIFY(UnderlyingReader_);
        return UnderlyingReader_->GetDecompressionStatistics();
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        YT_VERIFY(UnderlyingReader_);
        return UnderlyingReader_->GetFailedChunkIds();
    }

    virtual TInterruptDescriptor GetInterruptDescriptor(
        TRange<TUnversionedRow> unreadRows) const override
    {
        YT_ABORT();
    }

    virtual void Interrupt() override
    {
        YT_ABORT();
    }

    virtual void SkipCurrentReader() override
    {
        YT_ABORT();
    }

    virtual i64 GetTableRowIndex() const override
    {
        // Not supported.
        return -1;
    }

private:
    class TComparerBase
    {
    public:
        explicit TComparerBase(TSchemalessPartitionSortReader* reader)
            : KeyColumnCount_(reader->KeyColumnCount_)
            , KeyBuffer_(reader->KeyBuffer_)
        { }

    protected:
        int KeyColumnCount_;
        std::vector<TUnversionedValue>& KeyBuffer_;

        bool CompareRows(i64 lhs, i64 rhs) const
        {
            i64 lhsStartIndex = lhs * KeyColumnCount_;
            i64 lhsEndIndex   = lhsStartIndex + KeyColumnCount_;
            i64 rhsStartIndex = rhs * KeyColumnCount_;
            for (i64 lhsIndex = lhsStartIndex, rhsIndex = rhsStartIndex;
                lhsIndex < lhsEndIndex;
                ++lhsIndex, ++rhsIndex)
            {
                auto res = CompareRowValues(KeyBuffer_[lhsIndex], KeyBuffer_[rhsIndex]);
                if (res < 0)
                    return true;
                if (res > 0)
                    return false;
            }
            return false;
        }
    };

    class TSortComparer
        : public TComparerBase
    {
    public:
        explicit TSortComparer(TSchemalessPartitionSortReader* reader)
            : TComparerBase(reader)
        { }

        // Returns True iff row[lhs] < row[rhs]
        bool operator () (i32 lhs, i32 rhs) const
        {
            return CompareRows(lhs, rhs);
        }
    };

    class TMergeComparer
        : public TComparerBase
    {
    public:
        explicit TMergeComparer(TSchemalessPartitionSortReader* reader)
            : TComparerBase(reader)
            , Buckets_(reader->Buckets_)
        { }

        // Returns True iff row[Buckets[lhs]] < row[Buckets[rhs]]
        bool operator () (int lhs, int rhs) const
        {
            return CompareRows(Buckets_[lhs], Buckets_[rhs]);
        }

    private:
        std::vector<i32>& Buckets_;

    };

    template <class T>
    class TSafeVector
        : public std::vector<T>
    {
    public:
        explicit TSafeVector(TSchemalessPartitionSortReader* reader)
            : UnderlyingReader_(reader)
        { }

        void push_back(const T& value)
        {
            EnsureCapacity();
            std::vector<T>::push_back(value);
        }

        void push_back(T&& value)
        {
            EnsureCapacity();
            std::vector<T>::push_back(std::move(value));
        }

        using std::vector<T>::capacity;
        using std::vector<T>::reserve;
        using std::vector<T>::size;

    private:
        TSchemalessPartitionSortReader* UnderlyingReader_;

        void EnsureCapacity()
        {
            if (capacity() == size()) {
                UnderlyingReader_->SortQueueBarrier();
                reserve(static_cast<size_t>(size() * ReallocationFactor));
            }
        }

    };

    const TKeyColumns KeyColumns_;
    const int KeyColumnCount_;
    const TClosure OnNetworkReleased_;
    const TNameTablePtr NameTable_;

    const bool Approximate_;

    const i64 EstimatedRowCount_;
    int EstimatedBucketCount_;

    i64 TotalRowCount_ = 0;
    std::atomic<i64> SortedRowCount_ = {0};
    i64 ReadRowCount_ = 0;
    i64 ReadDataWeight_ = 0;

    TSafeVector<TUnversionedValue> KeyBuffer_;
    TSafeVector<TRowDescriptor> RowDescriptorBuffer_;
    TSafeVector<i32> Buckets_;
    TSafeVector<int> BucketStart_;

    std::vector<int> BucketHeap_;
    std::vector<i32> SortedIndexes_;

    TSortComparer SortComparer_;
    TMergeComparer MergeComparer_;

    TChunkedMemoryPool MemoryPool_;

    const TActionQueuePtr SortQueue_ = New<TActionQueue>("Sort");

    TPartitionMultiChunkReaderPtr UnderlyingReader_;

    // Sort error may occur due to CompositeValues in keys.
    std::vector<TFuture<void>> SortErrors_;

    TError MergeError_;
    std::atomic_bool MergeFinished_ = { false };

    void DoOpen()
    {
        InitInput();
        ReadInput();
        StartMerge();
    }

    void InitInput()
    {
        YT_LOG_INFO("Initializing input");
        PROFILE_TIMING ("/reduce/init_time") {
            EstimatedBucketCount_ = (EstimatedRowCount_ + SortBucketSize - 1) / SortBucketSize;
            YT_LOG_INFO("Input size estimated (RowCount: %v, BucketCount: %v)",
                EstimatedRowCount_,
                EstimatedBucketCount_);

            KeyBuffer_.reserve(EstimatedRowCount_ * KeyColumnCount_);
            RowDescriptorBuffer_.reserve(EstimatedRowCount_);
            Buckets_.reserve(EstimatedRowCount_ + EstimatedBucketCount_);
        }
    }

    void ReadInput()
    {
        YT_LOG_INFO("Started reading input");
        PROFILE_TIMING ("/reduce/read_time" ) {
            bool isNetworkReleased = false;

            int bucketId = 0;
            int bucketSize = 0;
            int rowIndex = 0;

            auto flushBucket = [&] () {
                Buckets_.push_back(BucketEndSentinel);
                BucketStart_.push_back(Buckets_.size());

                SortErrors_.push_back(InvokeSortBucket(bucketId));
                ++bucketId;
                bucketSize = 0;
            };

            BucketStart_.push_back(0);

            while (true) {
                i64 rowCount = 0;

                auto keyInserter = std::back_inserter(KeyBuffer_);
                auto rowDescriptorInserter = std::back_inserter(RowDescriptorBuffer_);

                auto result = UnderlyingReader_->Read(keyInserter, rowDescriptorInserter, &rowCount);
                if (!result)
                    break;

                if (rowCount == 0) {
                    WaitFor(UnderlyingReader_->GetReadyEvent())
                        .ThrowOnError();
                    continue;
                }

                // Push the row to the current bucket and flush the bucket if full.
                for (i64 i = 0; i < rowCount; ++i) {
                    Buckets_.push_back(rowIndex);
                    ++rowIndex;
                    ++bucketSize;
                }

                if (bucketSize >= SortBucketSize) {
                    flushBucket();
                }

                if (!isNetworkReleased && UnderlyingReader_->IsFetchingCompleted()) {
                    OnNetworkReleased_.Run();
                    isNetworkReleased =  true;
                }
            }

            if (bucketSize > 0) {
                flushBucket();
            }

            if (!isNetworkReleased) {
                YT_VERIFY(UnderlyingReader_->IsFetchingCompleted());
                OnNetworkReleased_.Run();
            }

            TotalRowCount_ = rowIndex;
            int bucketCount = static_cast<int>(BucketStart_.size()) - 1;

            if (!Approximate_) {
                YT_VERIFY(TotalRowCount_ <= EstimatedRowCount_);
                YT_VERIFY(bucketCount <= EstimatedBucketCount_);
            }

            YT_LOG_INFO("Finished reading input (RowCount: %v, BucketCount: %v)",
                TotalRowCount_,
                bucketCount);
        }
    }

    void DoSortBucket(int bucketId)
    {
        YT_LOG_DEBUG("Started sorting bucket %v", bucketId);

        int startIndex = BucketStart_[bucketId];
        int endIndex = BucketStart_[bucketId + 1] - 1;
        std::sort(Buckets_.begin() + startIndex, Buckets_.begin() + endIndex, SortComparer_);

        YT_LOG_DEBUG("Finished sorting bucket %v", bucketId);
    }

    void StartMerge()
    {
        YT_LOG_INFO("Waiting for sort thread");
        PROFILE_TIMING ("/reduce/sort_wait_time") {
            WaitFor(AllSucceeded(SortErrors_))
                .ThrowOnError();
        }
        YT_LOG_INFO("Sort thread is idle");

        SortedIndexes_.reserve(TotalRowCount_);

        for (int index = 0; index < static_cast<int>(BucketStart_.size()) - 1; ++index) {
            BucketHeap_.push_back(BucketStart_[index]);
        }

        MakeHeap(BucketHeap_.begin(), BucketHeap_.end(), MergeComparer_);

        SortedRowCount_ = 0;
        ReadRowCount_ = 0;

        InvokeMerge();
    }

    void DoMerge()
    {
        try {
            YT_LOG_INFO("Started merge");
            PROFILE_TIMING ("/reduce/merge_time") {
                int sortedRowCount = 0;
                while (!BucketHeap_.empty()) {
                    int bucketIndex = BucketHeap_.front();
                    if (SortedIndexes_.size() > 0) {
                        YT_ASSERT(!SortComparer_(Buckets_[bucketIndex], SortedIndexes_.back()));
                    }
                    SortedIndexes_.push_back(Buckets_[bucketIndex]);
                    ++bucketIndex;
                    if (Buckets_[bucketIndex] == BucketEndSentinel) {
                        ExtractHeap(BucketHeap_.begin(), BucketHeap_.end(), MergeComparer_);
                        BucketHeap_.pop_back();
                    } else {
                        BucketHeap_.front() = bucketIndex;
                        AdjustHeapFront(BucketHeap_.begin(), BucketHeap_.end(), MergeComparer_);
                    }

                    ++sortedRowCount;
                    if (sortedRowCount % RowsBetweenAtomicUpdate == 0) {
                        SortedRowCount_ = sortedRowCount;
                    }
                }

                YT_VERIFY(sortedRowCount == TotalRowCount_);
                SortedRowCount_ = sortedRowCount;
            }
            YT_LOG_INFO("Finished merge");
        } catch (const TErrorException& ex) {
            MergeError_ = ex;
        }

        MergeFinished_ = true;
    }

    void SortQueueBarrier()
    {
        BIND([] () { }).AsyncVia(SortQueue_->GetInvoker()).Run().Get();
    }

    TFuture<void> InvokeSortBucket(int bucketId)
    {
        return
            BIND(
                &TSchemalessPartitionSortReader::DoSortBucket,
                MakeWeak(this),
                bucketId)
            .AsyncVia(SortQueue_->GetInvoker())
            .Run();
    }

    void InvokeMerge()
    {
        SortQueue_->GetInvoker()->Invoke(BIND(
            &TSchemalessPartitionSortReader::DoMerge,
            MakeWeak(this)));
    }

};

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessPartitionSortReader(
    TMultiChunkReaderConfigPtr config,
    NApi::NNative::IClientPtr client,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TKeyColumns& keyColumns,
    TNameTablePtr nameTable,
    TClosure onNetworkReleased,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    i64 estimatedRowCount,
    bool approximate,
    int partitionTag,
    const TClientBlockReadOptions& blockReadOptions,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMultiReaderMemoryManagerPtr multiReaderMemoryManager)
{
    return New<TSchemalessPartitionSortReader>(
        config,
        client,
        blockCache,
        nodeDirectory,
        keyColumns,
        nameTable,
        onNetworkReleased,
        dataSourceDirectory,
        dataSliceDescriptors,
        estimatedRowCount,
        approximate,
        partitionTag,
        blockReadOptions,
        trafficMeter,
        bandwidthThrottler,
        rpsThrottler,
        std::move(multiReaderMemoryManager));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient

