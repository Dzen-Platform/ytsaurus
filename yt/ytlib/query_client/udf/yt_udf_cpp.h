#include "yt_udf_types.h"

#include <stdlib.h>

extern "C" char* AllocatePermanentBytes(TExecutionContext* context, size_t size);

extern "C" char* AllocateBytes(TExecutionContext* context, size_t size);
