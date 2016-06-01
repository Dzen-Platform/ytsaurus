#include "lz.h"

#include <yt/contrib/lz4/lz4.h>
#include <yt/contrib/lz4/lz4hc.h>

#include <yt/contrib/quicklz/quicklz.h>

namespace NYT {
namespace NCompression {
namespace {

////////////////////////////////////////////////////////////////////////////////

struct THeader
{
    i32 Signature;
    i32 InputSize;

    static const i32 CorrectSignature = (1 << 30) + 1;
};

struct TBlockHeader
{
    i32 OutputSize;
    i32 InputSize;
};

static_assert(sizeof(THeader) == sizeof(TBlockHeader),
    "Header and block header whould have the same size for compatibility reasons");

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NCompression
} // namespace NYT

Y_DECLARE_PODTYPE(NYT::NCompression::THeader);
Y_DECLARE_PODTYPE(NYT::NCompression::TBlockHeader);

namespace NYT {
namespace NCompression {

////////////////////////////////////////////////////////////////////////////////

struct TLzCompressedTag { };

size_t Lz4CompressionBound(const std::vector<int>& lengths)
{
    size_t bound = sizeof(THeader);
    for (auto length : lengths) {
        bound += sizeof(TBlockHeader);
        bound += LZ4_compressBound(length);
    }
    return bound;
}

void Lz4Compress(bool highCompression, StreamSource* source, TBlob* output)
{
    output->Resize(sizeof(THeader));
    size_t currentPos = output->Size();

    {
        THeader header;
        header.Signature = THeader::CorrectSignature;
        header.InputSize = source->Available();

        TMemoryOutput memoryOutput(output->Begin(), sizeof(THeader));
        WritePod(memoryOutput, header);
    }

    while (source->Available() > 0) {
        size_t len;
        const char* input = source->Peek(&len);

        // LZ4 only supports i32 length.
        len = std::min(len, static_cast<size_t>(1 << 30));

        size_t bound =
            currentPos +
            sizeof(TBlockHeader) +
            LZ4_compressBound(len);
        output->Resize(bound, false);

        size_t headerPos = currentPos;
        currentPos += sizeof(TBlockHeader);

        TBlockHeader header;
        header.InputSize = len;
        if (highCompression) {
            header.OutputSize = LZ4_compressHC(input, output->Begin() + currentPos, len);
        } else {
            header.OutputSize = LZ4_compress(input, output->Begin() + currentPos, len);
        }
        YCHECK(header.OutputSize >= 0);

        currentPos += header.OutputSize;
        output->Resize(currentPos);

        TMemoryOutput memoryOutput(output->Begin() + headerPos, sizeof(TBlockHeader));
        WritePod(memoryOutput, header);

        source->Skip(len);
    }
}

void Lz4Decompress(StreamSource* source, TBlob* output)
{
    if (source->Available() == 0) {
        return;
    }

    TBlockHeader startHeader;
    bool oldStyle = false;

    {
        THeader header;
        ReadPod(source, header);
        // COMPAT(ignat): for reading old-style blocks
        if (header.Signature != THeader::CorrectSignature) {
            oldStyle = true;
            startHeader.InputSize = header.InputSize;
            startHeader.OutputSize = header.Signature;
        } else {
            output->Reserve(header.InputSize);
        }
    }

    bool firstIter = true;
    while (source->Available() > 0) {
        TBlockHeader header;
        if (oldStyle && firstIter) {
            header = startHeader;
            firstIter = false;
        } else {
            ReadPod(source, header);
        }

        size_t outputPos = output->Size();
        size_t newSize = outputPos + header.InputSize;
        output->Resize(newSize, false);

        auto input = TBlob(TLzCompressedTag(), header.OutputSize, false);
        Read(source, input.Begin(), input.Size());

        YCHECK(LZ4_uncompress(input.Begin(), output->Begin() + outputPos, header.InputSize) >= 0);
    }
}

////////////////////////////////////////////////////////////////////////////////

void QuickLzCompress(StreamSource* source, TBlob* output)
{
    size_t currentPos = 0;
    while (source->Available() > 0) {
        qlz_state_compress state;

        size_t len;
        const char* input = source->Peek(&len);

        size_t bound =
            currentPos +
            sizeof(TBlockHeader) +
            /* compressed bound */(len + 400);
        output->Resize(bound, false);

        size_t headerPos = currentPos;
        currentPos += sizeof(TBlockHeader);

        TBlockHeader header;
        header.InputSize = len;
        header.OutputSize = qlz_compress(input, output->Begin() + currentPos, len, &state);
        YCHECK(header.OutputSize >= 0);

        currentPos += header.OutputSize;
        output->Resize(currentPos);

        TMemoryOutput memoryOutput(output->Begin() + headerPos, sizeof(TBlockHeader));
        WritePod(memoryOutput, header);

        source->Skip(len);
    }
}

void QuickLzDecompress(StreamSource* source, TBlob* output)
{
    while (source->Available() > 0) {
        qlz_state_decompress state;

        TBlockHeader header;
        ReadPod(source, header);

        size_t outputPos = output->Size();
        size_t newSize = outputPos + header.InputSize;
        output->Resize(newSize, false);

        auto input = TBlob(TLzCompressedTag(), header.OutputSize, false);
        Read(source, input.Begin(), input.Size());

        qlz_decompress(input.Begin(), output->Begin() + outputPos, &state);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCompression
} // namespace NYT

