#pragma once

#include "public.h"
#include "ref.h"

#include <util/stream/input.h>
#include <util/stream/output.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TChecksum GetChecksum(const TRef& data);

////////////////////////////////////////////////////////////////////////////////

class TChecksumInput
    : public TInputStream
{
public:
    explicit TChecksumInput(TInputStream* input);
    TChecksum GetChecksum() const;

protected:
    virtual size_t DoRead(void* buf, size_t len);

private:
    TInputStream* const Input_;
    TChecksum Checksum_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TChecksumOutput
    : public TOutputStream
{
public:
    explicit TChecksumOutput(TOutputStream* output);
    TChecksum GetChecksum() const;

protected:
    virtual void DoWrite(const void* buf, size_t len);
    virtual void DoFlush();
    virtual void DoFinish();

private:
    TOutputStream* const Output_;
    TChecksum Checksum_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
