#pragma once

#include "common.h"
#include "enum.h"

// Google Protobuf forward declarations.
namespace google {
namespace protobuf {

///////////////////////////////////////////////////////////////////////////////

class MessageLite;
class Message;

template <class Element>
class RepeatedField;
template <class Element>
class RepeatedPtrField;

///////////////////////////////////////////////////////////////////////////////

} // namespace protobuf
} // namespace google

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TError;
class TBloomFilter;
class TDataStatistics;

} // namespace NProto

namespace NLFAlloc {

class TLFAllocProfiler;

} // namespace NLFAlloc

struct TGuid;

template <class T>
class TErrorOr;

typedef TErrorOr<void> TError;

template <class T>
struct TErrorTraits;

class TStreamSaveContext;
class TStreamLoadContext;

template <class TSaveContext, class TLoadContext>
class TCustomPersistenceContext;

using TStreamPersistenceContext = TCustomPersistenceContext<
    TStreamSaveContext,
    TStreamLoadContext
>;

struct TValueBoundComparer;
struct TValueBoundSerializer;

template <class T, class C, class = void>
struct TSerializerTraits;

class TChunkedMemoryPool;

template <class TKey, class TComparer>
class TSkipList;

class TBlobOutput;

class TStringBuilder;

struct ICheckpointableInputStream;
struct ICheckpointableOutputStream;

DECLARE_REFCOUNTED_CLASS(TSlruCacheConfig)
DECLARE_REFCOUNTED_CLASS(TExpiringCacheConfig)

DECLARE_REFCOUNTED_CLASS(TLogDigestConfig)
DECLARE_REFCOUNTED_STRUCT(IDigest)

class TBloomFilterBuilder;
class TBloomFilter;

using TChecksum = ui64;
using TFingerprint = ui64;

template <class T, unsigned size>
class SmallVector;

template <class TProto>
class TRefCountedProto;

DECLARE_REFCOUNTED_CLASS(TProcess)

const ui32 YTCoreNoteType = 0x5f59545f; // = hex("_YT_") ;)
extern const TString YTCoreNoteName;

DECLARE_REFCOUNTED_CLASS(TCoreDumperConfig)
DECLARE_REFCOUNTED_CLASS(TCoreDumper)

///////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((OK)                 (0))
    ((Generic)            (1))
    ((Canceled)           (2))
    ((Timeout)            (3))
);

DEFINE_ENUM(EProcessErrorCode,
    ((NonZeroExitCode)    (10000))
    ((Signal)             (10001))
    ((CannotResolveBinary)(10002))
);

///////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMountTmpfsConfig)
DECLARE_REFCOUNTED_CLASS(TUmountConfig)

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT
