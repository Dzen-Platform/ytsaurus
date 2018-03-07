#pragma once

#include "public.h"

#include <util/stream/input.h>
#include <util/stream/output.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct ICheckpointableInputStream
    : public IInputStream
{
    virtual void SkipToCheckpoint() = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct ICheckpointableOutputStream
    : public IOutputStream
{
    virtual void MakeCheckpoint() = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! Wraps an input stream making it checkpointable.
std::unique_ptr<ICheckpointableInputStream> CreateCheckpointableInputStream(
    IInputStream* underlyingStream);

//! Wraps a given input stream constructing another one whose binary content
//! can be parsed by a checkpointable stream parser as a single block.
//! Used for migrating pre-0.17 snapshots that were not checkpointable.
std::unique_ptr<IInputStream> EscapsulateAsCheckpointableInputStream(
    IInputStream* underlyingStream);

//! Wraps an output stream making it checkpointable.
std::unique_ptr<ICheckpointableOutputStream> CreateCheckpointableOutputStream(
    IOutputStream* underlyingStream);

//! Wraps a checkpointable output stream adding some buffering.
std::unique_ptr<ICheckpointableOutputStream> CreateBufferedCheckpointableOutputStream(
    ICheckpointableOutputStream* underlyingStream,
    size_t bufferSize = 8192);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
