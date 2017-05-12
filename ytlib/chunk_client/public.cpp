#include "public.h"

#include <yt/ytlib/misc/workload.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

const TChunkId NullChunkId = NObjectClient::NullObjectId;
const TChunkListId NullChunkListId = NObjectClient::NullObjectId;
const TChunkTreeId NullChunkTreeId = NObjectClient::NullObjectId;

const TString DefaultStoreMediumName("default");
const TString DefaultCacheMediumName("cache");

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
