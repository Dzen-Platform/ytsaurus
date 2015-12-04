#pragma once

#include "common.h"

namespace NYT {
namespace NLFAlloc {

////////////////////////////////////////////////////////////////////////////////

const void* SafeMallocGetParam(const char* param);
void SafeMallocSetParam(const char* param, const void* value);

void SetBufferSize(i64);

i64 GetCurrentUsed();
i64 GetCurrentMmapped();
i64 GetCurrentMmappedCount();
i64 GetCurrentLargeBlocks();
i64 GetCurrentSmallBlocks();
i64 GetCurrentSystem();

i64 GetUserAllocated();
i64 GetMmapped();
i64 GetMmappedCount();
i64 GetMunmapped();
i64 GetMunmappedCount();
i64 GetSystemAllocated();
i64 GetSystemFreed();
i64 GetSmallBlocksAllocated();
i64 GetSmallBlocksFreed();
i64 GetLargeBlocksAllocated();
i64 GetLargeBlocksFreed();

////////////////////////////////////////////////////////////////////////////////

} // NLFAlloc
} // namespace NYT
