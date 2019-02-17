#pragma once

#include "common.h"
#include "enum.h"
#include "size_literals.h"
#include "intrusive_ptr.h"

// Google Protobuf forward declarations.
namespace google::protobuf {

////////////////////////////////////////////////////////////////////////////////

class Descriptor;
class MessageLite;
class Message;

template <class Element>
class RepeatedField;
template <class Element>
class RepeatedPtrField;

namespace io {

class ZeroCopyInputStream;
class ZeroCopyOutputStream;

} // namespace io

////////////////////////////////////////////////////////////////////////////////

} // namespace google::protobuf

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TError;
class TBloomFilter;
class TDataStatistics;

} // namespace NProto

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
DECLARE_REFCOUNTED_CLASS(TAsyncExpiringCacheConfig)

DECLARE_REFCOUNTED_CLASS(TLogDigestConfig)
DECLARE_REFCOUNTED_STRUCT(IDigest)

DECLARE_REFCOUNTED_STRUCT(TStrace)
DECLARE_REFCOUNTED_STRUCT(TStracerResult)

DECLARE_REFCOUNTED_STRUCT(TSignalerArg)

class TBloomFilterBuilder;
class TBloomFilter;

using TChecksum = ui64;
using TFingerprint = ui64;

constexpr TChecksum NullChecksum = 0;

template <class T, unsigned size>
class SmallVector;

template <class TProto, bool EnableWeak = false>
class TRefCountedProto;

DECLARE_REFCOUNTED_CLASS(TProcessBase)

const ui32 YTCoreNoteType = 0x5f59545f; // = hex("_YT_") ;)
extern const TString YTCoreNoteName;

DECLARE_REFCOUNTED_STRUCT(ICoreDumper)

////////////////////////////////////////////////////////////////////////////////

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

// Memory zone is used to pass hint to the allocator.
DEFINE_ENUM(EMemoryZone,
    ((Normal)     (0)) // default memory type
    ((Undumpable) (1)) // memory is omitted from the core dump
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMountTmpfsConfig)
DECLARE_REFCOUNTED_CLASS(TUmountConfig)
DECLARE_REFCOUNTED_CLASS(TExtractTarConfig)
DECLARE_REFCOUNTED_CLASS(TSetThreadPriorityConfig)
DECLARE_REFCOUNTED_CLASS(TFSQuotaConfig)
DECLARE_REFCOUNTED_CLASS(TChownChmodConfig)

DECLARE_REFCOUNTED_STRUCT(IMemoryChunkProvider)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
