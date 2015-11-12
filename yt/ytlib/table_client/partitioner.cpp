#include "partitioner.h"

#include <yt/core/misc/blob_output.h>
#include <yt/core/misc/common.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TOrderedPartitioner
    : public IPartitioner
{
public:
    explicit TOrderedPartitioner(const std::vector<TOwningKey>* keys)
        : Keys(keys)
    { }

    virtual int GetPartitionCount() override
    {
        return Keys->size() + 1;
    }

    virtual int GetPartitionIndex(const TUnversionedRow& row) override
    {
        auto it = std::upper_bound(
            Keys->begin(),
            Keys->end(),
            row,
            [] (const TUnversionedRow& row, const TOwningKey& element) {
                return row < element.Get();
            });
        return std::distance(Keys->begin(), it);
    }

private:
    const std::vector<TOwningKey>* Keys;

};

std::unique_ptr<IPartitioner> CreateOrderedPartitioner(const std::vector<TOwningKey>* keys)
{
    return std::unique_ptr<IPartitioner>(new TOrderedPartitioner(keys));
}

////////////////////////////////////////////////////////////////////////////////

class THashPartitioner
    : public IPartitioner
{
public:
    THashPartitioner(int partitionCount, int keyColumnCount)
        : PartitionCount(partitionCount)
        , KeyColumnCount(keyColumnCount)
    { }

    virtual int GetPartitionCount() override
    {
        return PartitionCount;
    }

    virtual int GetPartitionIndex(const TUnversionedRow& row) override
    {
        return GetHash(row, KeyColumnCount) % PartitionCount;
    }

private:
    int PartitionCount;
    int KeyColumnCount;

};

std::unique_ptr<IPartitioner> CreateHashPartitioner(int partitionCount, int keyColumnCount)
{
    return std::unique_ptr<IPartitioner>(new THashPartitioner(partitionCount, keyColumnCount));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
