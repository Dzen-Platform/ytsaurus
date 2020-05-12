#pragma once

#include "public.h"

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/misc/small_vector.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IPartitioner
    : public virtual TRefCounted
{
    virtual int GetPartitionCount() = 0;
    virtual int GetPartitionIndex(TUnversionedRow row) = 0;
};

DEFINE_REFCOUNTED_TYPE(IPartitioner)

IPartitionerPtr CreateOrderedPartitioner(const TSharedRef& wirePartitionKeys);
IPartitionerPtr CreateHashPartitioner(int partitionCount, int keyColumnCount);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
