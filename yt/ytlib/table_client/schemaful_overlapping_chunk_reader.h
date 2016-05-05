#include "public.h"

#include <functional>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

// NB: Rows are allocated in row merger buffer which is cleared on each Read() call.

constexpr int DefaultMinConcurrentOverlappingReaders = 5;

using TOverlappingReaderKeyComparer = std::function<int(
    const TUnversionedValue*,
    const TUnversionedValue*,
    const TUnversionedValue*,
    const TUnversionedValue*)>;

ISchemafulReaderPtr CreateSchemafulOverlappingLookupChunkReader(
    TSchemafulRowMergerPtr rowMerger,
    std::function<IVersionedReaderPtr()> readerFactory);

ISchemafulReaderPtr CreateSchemafulOverlappingRangeChunkReader(
    const std::vector<TOwningKey>& boundaries,
    TSchemafulRowMergerPtr rowMerger,
    std::function<IVersionedReaderPtr(int index)> readerFactory,
    TOverlappingReaderKeyComparer keyComparer,
    int minConcurrentReaders = DefaultMinConcurrentOverlappingReaders);

IVersionedReaderPtr CreateVersionedOverlappingRangeChunkReader(
    const std::vector<TOwningKey>& boundaries,
    TVersionedRowMergerPtr rowMerger,
    std::function<IVersionedReaderPtr(int index)> readerFactory,
    TOverlappingReaderKeyComparer keyComparer,
    int minConcurrentReaders = DefaultMinConcurrentOverlappingReaders);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
