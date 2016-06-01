#pragma once

#include "blob.h"
#include "ref.h"
#include "small_vector.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkType>
TChunkType GetChunkMask(int bitIndex, bool value)
{
    return static_cast<TChunkType>(value) << (bitIndex % (sizeof(TChunkType) * 8));
}

////////////////////////////////////////////////////////////////////////////////

template <class TChunkType, int DefaultChunkCount = 1>
class TAppendOnlyBitmap
{
public:
    explicit TAppendOnlyBitmap(int bitCapacity = 0)
    {
        YCHECK(bitCapacity >= 0);
        if (bitCapacity) {
            Data_.reserve((bitCapacity - 1) / sizeof(TChunkType) / 8 + 1);
        }
    }

    void Append(bool value)
    {
        if (Data_.size() * sizeof(TChunkType) * 8 == BitSize_) {
            Data_.push_back(TChunkType());
        }

        Data_.back() |= GetChunkMask<TChunkType>(BitSize_, value);
        ++BitSize_;
    }

    bool operator[] (i64 index) const
    {
        Y_ASSERT(index < BitSize_);
        int dataIndex = index / (sizeof(TChunkType) * 8);
        return static_cast<bool>(Data_[dataIndex] & GetChunkMask<TChunkType>(index, true));
    }

    i64 GetBitSize() const
    {
        return BitSize_;
    }

    template <class TTag>
    TSharedRef Flush()
    {
        auto blob = TBlob(TTag(), Data_.data(), Size());
        return TSharedRef::FromBlob(std::move(blob));
    }

    const TChunkType* Data() const
    {
        return Data_.data();
    }

    int Size() const
    {
        return Data_.size() * sizeof(TChunkType);
    }

private:
    i64 BitSize_ = 0;
    SmallVector<TChunkType, DefaultChunkCount> Data_;

};

////////////////////////////////////////////////////////////////////////////////

template <class TChunkType>
class TReadOnlyBitmap
{
public:
    TReadOnlyBitmap()
        : Data_(nullptr)
        , BitSize_(0)
    { }

    TReadOnlyBitmap(const TChunkType* data, int bitSize)
    {
        Reset(data, bitSize);
    }

    void Reset(const TChunkType* data, int bitSize)
    {
        YCHECK(data);
        YCHECK(bitSize >= 0);
        Data_ = data;
        BitSize_ = bitSize;
    }

    bool operator[] (int index) const
    {
        Y_ASSERT(index < BitSize_);
        int dataIndex = index / (sizeof(TChunkType) * 8);
        return static_cast<bool>(Data_[dataIndex] & GetChunkMask<TChunkType>(index, true));
    }

    int GetByteSize() const
    {
        int chunkSize = sizeof(TChunkType) * 8;
        int sizeInChunks = BitSize_ / chunkSize + (BitSize_ % chunkSize ? 1 : 0);
        return sizeInChunks * sizeof(TChunkType);
    }

private:
    const TChunkType* Data_;
    int BitSize_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
