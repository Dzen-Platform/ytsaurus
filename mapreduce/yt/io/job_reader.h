#pragma once

#include "proxy_input.h"

#include <util/stream/buffered.h>
#include <util/stream/pipe.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TJobReader
    : public TProxyInput
{
public:
    explicit TJobReader(int fd);

    virtual bool OnStreamError(const yexception& e, ui32 rangeIndex, ui64 rowIndex) override;
    virtual bool HasRangeIndices() const override { return false; }

protected:
    virtual size_t DoRead(void* buf, size_t len) override;

private:
    int Fd_;
    TPipedInput PipedInput_;
    TBufferedInput BufferedInput_;

    static const size_t BUFFER_SIZE = 64 << 10;
};

////////////////////////////////////////////////////////////////////////////////


} // namespace NYT
