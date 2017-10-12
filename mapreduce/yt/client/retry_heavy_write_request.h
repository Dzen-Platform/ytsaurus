#pragma once

#include <mapreduce/yt/http/requests.h>

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

void RetryHeavyWriteRequest(
    const TAuth& auth,
    const TTransactionId& parentId,
    THttpHeader& header,
    std::function<THolder<IInputStream>()> streamMaker);

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT
