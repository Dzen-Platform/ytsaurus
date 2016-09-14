#pragma once

#include <yt/core/misc/public.h>
#include <yt/core/misc/string.h>

namespace NYT {
namespace NCompression {

///////////////////////////////////////////////////////////////////////////////

struct ICodec;

DEFINE_ENUM_WITH_UNDERLYING_TYPE(ECodec, i8,
    ((None)                       (0))
    ((Snappy)                     (1))
    ((Lz4)                        (4))
    ((Lz4HighCompression)         (5))
    ((QuickLz)                    (6))

    ((Brotli_1)                  (11))
    ((Brotli_2)                  (12))
    ((Brotli_3)                   (8))
    ((Brotli_4)                  (13))
    ((Brotli_5)                   (9))
    ((Brotli_6)                  (14))
    ((Brotli_7)                  (15))
    ((Brotli_8)                  (10))
    ((Brotli_9)                  (16))
    ((Brotli_10)                 (17))
    ((Brotli_11)                 (18))

    ((Zlib_1)                    (19))
    ((Zlib_2)                    (20))
    ((Zlib_3)                    (21))
    ((Zlib_4)                    (22))
    ((Zlib_5)                    (23))
    ((Zlib_6)                     (2))
    ((Zlib_7)                    (24))
    ((Zlib_8)                    (25))
    ((Zlib_9)                     (3))

    ((Zstd_1)                    (26))
    ((Zstd_2)                    (27))
    ((Zstd_3)                    (28))
    ((Zstd_4)                    (29))
    ((Zstd_5)                    (30))
    ((Zstd_6)                    (31))
    ((Zstd_7)                    (32))
    ((Zstd_8)                    (33))
    ((Zstd_9)                    (34))
    ((Zstd_10)                   (35))
    ((Zstd_11)                   (36))
    ((Zstd_12)                   (37))
    ((Zstd_13)                   (38))
    ((Zstd_14)                   (39))
    ((Zstd_15)                   (40))
    ((Zstd_16)                   (41))
    ((Zstd_17)                   (42))
    ((Zstd_18)                   (43))
    ((Zstd_19)                   (44))
    ((Zstd_20)                   (45))
    ((Zstd_21)                   (46))

    ((Lzma_0)                    (47))
    ((Lzma_1)                    (48))
    ((Lzma_2)                    (49))
    ((Lzma_3)                    (50))
    ((Lzma_4)                    (51))
    ((Lzma_5)                    (52))
    ((Lzma_6)                    (53))
    ((Lzma_7)                    (54))
    ((Lzma_8)                    (55))
    ((Lzma_9)                    (56))

    // Deprecated
    ((Zlib6)                      (2))
    ((GzipNormal)                 (2))
    ((Zlib9)                      (3))
    ((GzipBestCompression)        (3))
    ((ZstdLegacy)                 (7))
    ((Zstd)                      (28))
    ((Brotli3)                    (8))
    ((Brotli5)                    (9))
    ((Brotli8)                   (10))
);

const int MaxBlockSize = 1 << 30;

///////////////////////////////////////////////////////////////////////////////

} // namespace NCompression
} // namespace NYT
