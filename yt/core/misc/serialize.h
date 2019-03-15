#pragma once

#include "public.h"
#include "align.h"
#include "assert.h"
#include "enum.h"
#include "error.h"
#include "guid.h"
#include "mpl.h"
#include "optional.h"
#include "variant.h"
#include "property.h"
#include "ref.h"
#include "serialize_dump.h"

#include <util/stream/file.h>
#include <util/stream/input.h>
#include <util/stream/output.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TOutput>
void Write(TOutput& output, TRef ref)
{
    output.Write(ref.Begin(), ref.Size());
}

template <class TInput>
size_t Read(TInput& input, TRef& ref)
{
    auto loadBytes = input.Load(ref.Begin(), ref.Size());
    YCHECK(loadBytes == ref.Size());
    return loadBytes;
}

template <class TOutput, class T>
void WritePod(TOutput& output, const T& obj)
{
    static_assert(TTypeTraits<T>::IsPod, "T must be a pod-type.");
    output.Write(&obj, sizeof(obj));
}

template <class TInput, class T>
void ReadPod(TInput& input, T& obj, bool safe = false)
{
    static_assert(TTypeTraits<T>::IsPod, "T must be a pod-type.");
    auto loadBytes = input.Load(&obj, sizeof(obj));
    if (safe) {
        if (loadBytes != sizeof(obj)) {
            THROW_ERROR_EXCEPTION("Byte size mismatch while reading a pod")
                << TErrorAttribute("bytes_loaded", loadBytes)
                << TErrorAttribute("bytes_expected", sizeof(obj));
        }
    } else {
        YCHECK(loadBytes == sizeof(obj));
    }
}

template <class TOutput>
size_t WritePadding(TOutput& output, size_t writtenSize)
{
    output.Write(&NDetail::SerializationPadding, GetPaddingSize(writtenSize));
    return AlignUp(writtenSize);
}

template <class TOutput>
size_t WritePadded(TOutput& output, TRef ref)
{
    output.Write(ref.Begin(), ref.Size());
    output.Write(&NYT::NDetail::SerializationPadding, GetPaddingSize(ref.Size()));
    return AlignUp(ref.Size());
}

template <class TInput>
size_t ReadPadded(TInput& input, const TMutableRef& ref)
{
    auto loadBytes = input.Load(ref.Begin(), ref.Size());
    YCHECK(loadBytes == ref.Size());
    input.Skip(GetPaddingSize(ref.Size()));
    return AlignUp(ref.Size());
}

template <class TInput, class T>
size_t ReadPodPadded(TInput& input, T& obj)
{
    static_assert(TTypeTraits<T>::IsPod, "T must be a pod-type.");
    return ReadPadded(input, TMutableRef::FromPod(obj));
}

template <class TOutput, class T>
size_t WritePodPadded(TOutput& output, const T& obj)
{
    static_assert(TTypeTraits<T>::IsPod, "T must be a pod-type.");
    return WritePadded(output, TRef::FromPod(obj));
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
TSharedRef PackRefs(const T& parts)
{
    size_t size = 0;

    // Number of bytes to hold vector size.
    size += sizeof(i32);
    // Number of bytes to hold ref sizes.
    size += sizeof(i64) * parts.size();
    // Number of bytes to hold refs.
    for (const auto& ref : parts) {
        size += ref.Size();
    }

    struct TPackedRefsTag { };
    auto result = TSharedMutableRef::Allocate<TPackedRefsTag>(size, false);
    TMemoryOutput output(result.Begin(), result.Size());

    WritePod(output, static_cast<i32>(parts.size()));
    for (const auto& ref : parts) {
        WritePod(output, static_cast<i64>(ref.Size()));
        Write(output, ref);
    }

    return result;
}

// XXX(kiselyovp) unittests for safe unpacking if this gets approved
template <class T>
void UnpackRefs(const TSharedRef& packedRef, T* parts, bool safe = false)
{
    TMemoryInput input(packedRef.Begin(), packedRef.Size());

    i32 size;
    ReadPod(input, size, safe);
    if (safe) {
        if (size < 0) {
            THROW_ERROR_EXCEPTION("Packed ref size is negative")
                << TErrorAttribute("size", size);
        }
    } else {
        YCHECK(size >= 0);
    }

    parts->clear();
    parts->reserve(size);

    for (int index = 0; index < size; ++index) {
        i64 partSize;
        ReadPod(input, partSize, safe);
        if (safe) {
            if (partSize < 0) {
                THROW_ERROR_EXCEPTION("A part of a packed ref has negative size")
                    << TErrorAttribute("index", index)
                    << TErrorAttribute("size", partSize);
            }
            if (packedRef.End() - input.Buf() < partSize) {
                THROW_ERROR_EXCEPTION("A part of a packed ref is too large")
                    << TErrorAttribute("index", index)
                    << TErrorAttribute("size", partSize)
                    << TErrorAttribute("bytes_left", packedRef.End() - input.Buf());
            }
        } else {
            YCHECK(partSize >= 0);
            YCHECK(packedRef.End() - input.Buf() >= partSize);
        }

        parts->push_back(packedRef.Slice(input.Buf(), input.Buf() + partSize));

        input.Skip(partSize);
    }

    if (safe) {
        if (input.Buf() < packedRef.End()) {
            THROW_ERROR_EXCEPTION("Packed ref is too large")
                << TErrorAttribute("extra_bytes", packedRef.End() - input.Buf());
        }
    } else {
        YCHECK(input.Buf() == packedRef.End());
    }
}

template <class TTag, class TParts>
TSharedRef MergeRefsToRef(const TParts& parts)
{
    size_t size = GetByteSize(parts);
    auto packedRef = TSharedMutableRef::Allocate<TTag>(size, false);
    size_t pos = 0;
    for (const auto& part : parts) {
        std::copy(part.Begin(), part.End(), packedRef.Begin() + pos);
        pos += part.Size();
    }
    return packedRef;
}

template <class TParts>
TString MergeRefsToString(const TParts& parts)
{
    size_t size = GetByteSize(parts);
    TString packedString;
    packedString.reserve(size);
    for (const auto& part : parts) {
        packedString.append(part.Begin(), part.End());
    }
    return packedString;
}

////////////////////////////////////////////////////////////////////////////////

class TStreamSaveContext
{
public:
    DEFINE_BYVAL_RW_PROPERTY(IOutputStream*, Output);
    DEFINE_BYVAL_RW_PROPERTY(int, Version);

public:
    TStreamSaveContext();
    explicit TStreamSaveContext(IOutputStream* output);

};

////////////////////////////////////////////////////////////////////////////////

class TStreamLoadContext
{
public:
    DEFINE_BYVAL_RW_PROPERTY(IInputStream*, Input);
    DEFINE_BYREF_RW_PROPERTY(TSerializationDumper, Dumper);
    DEFINE_BYVAL_RW_PROPERTY(int, Version);

public:
    TStreamLoadContext();
    explicit TStreamLoadContext(IInputStream* input);

};

////////////////////////////////////////////////////////////////////////////////

template <class TSaveContext, class TLoadContext>
class TCustomPersistenceContext
{
public:
    TCustomPersistenceContext(TSaveContext& context)
        : SaveContext_(&context)
        , LoadContext_(nullptr)
    { }

    TCustomPersistenceContext(TLoadContext& context)
        : SaveContext_(nullptr)
        , LoadContext_(&context)
    { }

    bool IsSave() const
    {
        return SaveContext_ != nullptr;
    }

    TSaveContext& SaveContext() const
    {
        Y_ASSERT(SaveContext_);
        return *SaveContext_;
    }

    bool IsLoad() const
    {
        return LoadContext_ != nullptr;
    }

    TLoadContext& LoadContext() const
    {
        Y_ASSERT(LoadContext_);
        return *LoadContext_;
    }

    template <class TOtherContext>
    operator TOtherContext() const
    {
        return IsSave() ? TOtherContext(*SaveContext_) : TOtherContext(*LoadContext_);
    }

    int GetVersion() const
    {
        return IsSave() ? SaveContext().GetVersion() : LoadContext().GetVersion();
    }

private:
    TSaveContext* const SaveContext_;
    TLoadContext* const LoadContext_;

};

////////////////////////////////////////////////////////////////////////////////

template <class T, class C, class... TArgs>
void Save(C& context, const T& value, TArgs&&... args);

template <class T, class C, class... TArgs>
void Load(C& context, T& value, TArgs&&... args);

template <class T, class C, class... TArgs>
T Load(C& context, TArgs&&... args);

////////////////////////////////////////////////////////////////////////////////
// TODO(babenko): move to inl

template <class T, class C, class... TArgs>
void Save(C& context, const T& value, TArgs&&... args)
{
    TSerializerTraits<T, C>::TSerializer::Save(context, value, std::forward<TArgs>(args)...);
}

template <class T, class C, class... TArgs>
void Load(C& context, T& value, TArgs&&... args)
{
    TSerializerTraits<T, C>::TSerializer::Load(context, value, std::forward<TArgs>(args)...);
}

template <class T, class C, class... TArgs>
T Load(C& context, TArgs&&... args)
{
    T value;
    Load(context, value, std::forward<TArgs>(args)...);
    return value;
}

template <class T, class C, class... TArgs>
T LoadSuspended(C& context, TArgs&&... args)
{
    SERIALIZATION_DUMP_SUSPEND(context) {
        return Load<T, C, TArgs...>(context, std::forward<TArgs>(args)...);
    }
}

template <class S, class T, class C>
void Persist(const C& context, T& value)
{
    if (context.IsSave()) {
        S::Save(context.SaveContext(), value);
    } else if (context.IsLoad()) {
        S::Load(context.LoadContext(), value);
    } else {
        Y_UNREACHABLE();
    }
}

struct TDefaultSerializer;

template <class T, class C>
void Persist(const C& context, T& value)
{
    Persist<TDefaultSerializer, T, C>(context, value);
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_MPL_MEMBER_DETECTOR(Persist);

template <class T, class = void>
struct TPersistMemberTraits
{ };

template <class T>
struct TPersistMemberTraits<T, typename NMpl::TEnableIfC<THasPersistMember<T>::Value>::TType>
{
    template <class S>
    struct TSignatureCracker
    { };

    template <class U, class C>
    struct TSignatureCracker<void (U::*)(C&)>
    {
        typedef C TContext;
    };

    typedef typename TSignatureCracker<decltype(&T::Persist)>::TContext TContext;
};

////////////////////////////////////////////////////////////////////////////////
// Simple types

struct TValueBoundSerializer
{
    template <class T, class C, class = void>
    struct TSaver
    {
        static void Do(C& context, const T& value)
        {
            value.Save(context);
        }
    };

    template <class T, class C, class = void>
    struct TLoader
    {
        static void Do(C& context, T& value)
        {
            value.Load(context);
        }
    };

    template <class T, class C>
    struct TSaver<T, C, typename NMpl::TEnableIfC<THasPersistMember<T>::Value>::TType>
    {
        static void Do(C& context, const T& value)
        {
            typename TPersistMemberTraits<T>::TContext wrappedContext(context);
            const_cast<T&>(value).Persist(wrappedContext);
        }
    };

    template <class T, class C>
    struct TLoader<T, C, typename NMpl::TEnableIfC<THasPersistMember<T>::Value>::TType>
    {
        static void Do(C& context, T& value)
        {
            typename TPersistMemberTraits<T>::TContext wrappedContext(context);
            value.Persist(wrappedContext);
        }
    };


    template <class T, class C>
    static void Save(C& context, const T& value)
    {
        TSaver<T, C>::Do(context, value);
    }

    template <class T, class C>
    static void Load(C& context, T& value)
    {
        TLoader<T, C>::Do(context, value);
    }
};

struct TDefaultSerializer
{
    template <class T, class C>
    static void Save(C& context, const T& value)
    {
        NYT::Save(context, value);
    }

    template <class T, class C>
    static void Load(C& context, T& value)
    {
        NYT::Load(context, value);
    }
};

struct TRangeSerializer
{
    template <class C>
    static void Save(C& context, TRef value)
    {
        auto* output = context.GetOutput();
        output->Write(value.Begin(), value.Size());
    }

    template <class C>
    static void Load(C& context, const TMutableRef& value)
    {
        auto* input = context.GetInput();
        YCHECK(input->Load(value.Begin(), value.Size()) == value.Size());

        SERIALIZATION_DUMP_WRITE(context, "raw[%v] %v", value.Size(), DumpRangeToHex(value));
    }
};

struct TPodSerializer
{
    template <class T, class C>
    static void Save(C& context, const T& value)
    {
        TRangeSerializer::Save(context, TRef::FromPod(value));
    }

    template <class T, class C>
    static void Load(C& context, T& value)
    {
        SERIALIZATION_DUMP_SUSPEND(context) {
            TRangeSerializer::Load(context, TMutableRef::FromPod(value));
        }
        TSerializationDumpPodWriter<T>::Do(context, value);
    }
};

struct TSizeSerializer
{
    template <class C>
    static void Save(C& context, size_t value)
    {
        ui32 fixedValue = static_cast<ui32>(value);
        TPodSerializer::Save(context, fixedValue);
    }

    template <class C>
    static void Load(C& context, size_t& value)
    {
        ui32 fixedValue;
        TPodSerializer::Load(context, fixedValue);
        value = static_cast<size_t>(fixedValue);
    }


    // Helpers.
    template <class C>
    static size_t Load(C& context)
    {
        size_t value;
        Load(context, value);
        return value;
    }

    template <class C>
    static size_t LoadSuspended(C& context)
    {
        SERIALIZATION_DUMP_SUSPEND(context) {
            return Load(context);
        }
    }
};

struct TSharedRefSerializer
{
    template <class C>
    static void Save(C& context, const TSharedRef& value)
    {
        TSizeSerializer::Save(context, value.Size());

        auto* output = context.GetOutput();
        output->Write(value.Begin(), value.Size());
    }

    template <class C>
    static void Load(C& context, TSharedRef& value)
    {
        return Load(context, value, TDefaultSharedBlobTag());
    }

    template <class C, class TTag>
    static void Load(C& context, TSharedRef& value, TTag)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        auto mutableValue = TSharedMutableRef::Allocate<TTag>(size, false);

        auto* input = context.GetInput();
        YCHECK(input->Load(mutableValue.Begin(), mutableValue.Size()) == mutableValue.Size());
        value = mutableValue;

        SERIALIZATION_DUMP_WRITE(context, "TSharedRef %v", DumpRangeToHex(value));
    }
};

struct TSharedRefArraySerializer
{
    template <class C>
    static void Save(C& context, const TSharedRefArray& value)
    {
        TSizeSerializer::Save(context, value.Size());

        for (const auto& part : value) {
            Save(context, part);
        }
    }

    template <class C>
    static void Load(C& context, TSharedRefArray& value)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        std::vector<TSharedRef> parts(size);

        SERIALIZATION_DUMP_WRITE(context, "TSharedRefArray[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (int index = 0; index < static_cast<int>(size); ++index) {
                SERIALIZATION_DUMP_SUSPEND(context) {
                    Load(context, parts[index]);
                }
                SERIALIZATION_DUMP_WRITE(context, "%v => %v", index, DumpRangeToHex(parts[index]));
            }
        }

        value = TSharedRefArray(std::move(parts));
    }
};

struct TEnumSerializer
{
    template <class T, class C>
    static void Save(C& context, const T& value)
    {
        NYT::Save(context, static_cast<i32>(value));
    }

    template <class T, class C>
    static void Load(C& context, T& value)
    {
        SERIALIZATION_DUMP_SUSPEND(context) {
            value = T(NYT::Load<i32>(context));
        }

        SERIALIZATION_DUMP_WRITE(context, "%v %v", TEnumTraits<T>::GetTypeName(), value);
    }
};

struct TStringSerializer
{
    template <class C>
    static void Save(C& context, const TString& value)
    {
        TSizeSerializer::Save(context, value.size());

        TRangeSerializer::Save(context, TRef::FromString(value));
    }

    template <class C>
    static void Load(C& context, TString& value)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        value.resize(size);

        SERIALIZATION_DUMP_SUSPEND(context) {
            TRangeSerializer::Load(context, TMutableRef::FromString(value));
        }

        SERIALIZATION_DUMP_WRITE(context, "TString %Qv", value);
    }
};

template <class TUnderlyingSerializer = TDefaultSerializer>
struct TOptionalSerializer
{
    template <class T, class C>
    static void Save(C& context, const std::optional<T>& optional)
    {
        using NYT::Save;

        Save(context, optional.operator bool());

        if (optional) {
            TUnderlyingSerializer::Save(context, *optional);
        }
    }

    template <class T, class C>
    static void Load(C& context, std::optional<T>& optional)
    {
        using NYT::Load;

        bool hasValue = LoadSuspended<bool>(context);

        if (hasValue) {
            T temp;
            TUnderlyingSerializer::Load(context, temp);
            optional = temp;
        } else {
            optional.reset();
            SERIALIZATION_DUMP_WRITE(context, "null");
        }
    }
};

template <size_t Index, class... Ts>
struct TVariantSerializerTraits;

template <size_t Index, class T, class... Ts>
struct TVariantSerializerTraits<Index, T, Ts...>
{
    template <class C, class V>
    static void Save(C& context, const V& variant)
    {
        if (Index == variant.index()) {
            NYT::Save(context, std::get<T>(variant));
        } else {
            TVariantSerializerTraits<Index + 1, Ts...>::Save(context, variant);
        }
    }

    template <class C, class V>
    static void Load(C& context, size_t index, V& variant)
    {
        if (Index == index) {
            variant = V(std::in_place_index_t<Index>());
            NYT::Load(context, std::get<T>(variant));
        } else {
            TVariantSerializerTraits<Index + 1, Ts...>::Load(context, index, variant);
        }
    }
};

template <size_t Index>
struct TVariantSerializerTraits<Index>
{
    template <class C, class V>
    static void Save(C& /*context*/, const V& /*variant*/)
    {
        // Invalid variant index.
        Y_UNREACHABLE();
    }

    template <class C, class V>
    static void Load(C& /*context*/, size_t /*index*/, V& /*variant*/)
    {
        // Invalid variant index.
        Y_UNREACHABLE();
    }
};

struct TVariantSerializer
{
    template <class... Ts, class C>
    static void Save(C& context, const std::variant<Ts...>& variant)
    {
        NYT::Save<ui32>(context, variant.index());
        TVariantSerializerTraits<0, Ts...>::Save(context, variant);
    }

    template <class... Ts, class C>
    static void Load(C& context, std::variant<Ts...>& variant)
    {
        size_t index = NYT::Load<ui32>(context);
        TVariantSerializerTraits<0, Ts...>::Load(context, index, variant);
    }
};

template <class TUnderlyingSerializer = TDefaultSerializer>
struct TAtomicSerializer
{
    template <class T, class C>
    static void Save(C& context, const std::atomic<T>& value)
    {
        TUnderlyingSerializer::Save(context, value.load());
    }

    template <class T, class C>
    static void Load(C& context, std::atomic<T>& value)
    {
        T temp;
        TUnderlyingSerializer::Load(context, temp);
        value.store(std::move(temp));
    }
};

////////////////////////////////////////////////////////////////////////////////
// Sorters

template <class T, class C>
class TNoopSorter
{
public:
    typedef typename T::const_iterator TIterator;

    explicit TNoopSorter(const T& any)
        : Any_(any)
    { }

    TIterator begin()
    {
        return Any_.begin();
    }

    TIterator end()
    {
        return Any_.end();
    }

private:
    const T& Any_;

};

template <class C>
struct TValueSorterComparer
{
    template <class TIterator>
    static bool Compare(TIterator lhs, TIterator rhs)
    {
        typedef typename std::remove_const<typename std::remove_reference<decltype(*lhs)>::type>::type T;
        typedef typename TSerializerTraits<T, C>::TComparer TComparer;
        return TComparer::Compare(*lhs, *rhs);
    }
};

template <class C>
struct TKeySorterComparer
{
    template <class TIterator>
    static bool Compare(TIterator lhs, TIterator rhs)
    {
        typedef typename std::remove_const<typename std::remove_reference<decltype(lhs->first)>::type>::type T;
        typedef typename TSerializerTraits<T, C>::TComparer TComparer;
        return TComparer::Compare(lhs->first, rhs->first);
    }
};

template <class C>
struct TKeyValueSorterComparer
{
    template <class TIterator>
    static bool Compare(TIterator lhs, TIterator rhs)
    {
        typedef typename std::remove_const<typename std::remove_reference<decltype(lhs->first)>::type>::type TKey;
        typedef typename std::remove_const<typename std::remove_reference<decltype(lhs->second)>::type>::type TValue;
        typedef typename TSerializerTraits<TKey, C>::TComparer TKeyComparer;
        typedef typename TSerializerTraits<TValue, C>::TComparer TValueComparer;
        if (TKeyComparer::Compare(lhs->first, rhs->first)) {
            return true;
        }
        if (TKeyComparer::Compare(rhs->first, lhs->first)) {
            return false;
        }
        return TValueComparer::Compare(lhs->second, rhs->second);
    }
};

template <class T, class Q>
class TCollectionSorter
{
public:
    using TIterator = typename T::const_iterator;
    using TIterators = SmallVector<TIterator, 16>;

    class TIteratorWrapper
    {
    public:
        TIteratorWrapper(const TIterators* iterators, size_t index)
            : Iterators_(iterators)
            , Index_(index)
        { }

        const typename T::value_type& operator * ()
        {
            return *((*Iterators_)[Index_]);
        }

        TIteratorWrapper& operator ++ ()
        {
            ++Index_;
            return *this;
        }

        bool operator == (const TIteratorWrapper& other) const
        {
            return Index_ == other.Index_;
        }

        bool operator != (const TIteratorWrapper& other) const
        {
            return Index_ != other.Index_;
        }

    private:
        const TIterators* const Iterators_;
        size_t Index_;

    };

    explicit TCollectionSorter(const T& set)
    {
        Iterators_.reserve(set.size());
        for (auto it = set.begin(); it != set.end(); ++it) {
            Iterators_.push_back(it);
        }

        std::sort(
            Iterators_.begin(),
            Iterators_.end(),
            [] (TIterator lhs, TIterator rhs) {
                return Q::Compare(lhs, rhs);
            });
    }

    TIteratorWrapper begin() const
    {
        return TIteratorWrapper(&Iterators_, 0);
    }

    TIteratorWrapper end() const
    {
        return TIteratorWrapper(&Iterators_, Iterators_.size());
    }

private:
    TIterators Iterators_;

};

struct TSortedTag { };
struct TUnsortedTag { };

template <class T, class C, class TTag>
struct TSorterSelector
{ };

template <class T, class C>
struct TSorterSelector<T, C, TUnsortedTag>
{
    typedef TNoopSorter<T, C> TSorter;
};

template <class T, class C>
struct TSorterSelector<std::vector<T>, C, TSortedTag>
{
    typedef TCollectionSorter<std::vector<T>, TValueSorterComparer<C>> TSorter;
};

template <class T, class C, unsigned size>
struct TSorterSelector<SmallVector<T, size>, C, TSortedTag>
{
    typedef TCollectionSorter<SmallVector<T, size>, TValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::set<T...>, C, TSortedTag>
{
    typedef TNoopSorter<std::set<T...>, C> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::map<T...>, C, TSortedTag>
{
    typedef TNoopSorter<std::map<T...>, C> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::unordered_set<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<std::unordered_set<T...>, TValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<THashSet<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<THashSet<T...>, TValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::unordered_multiset<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<std::unordered_multiset<T...>, TValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<THashMultiSet<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<THashMultiSet<T...>, TValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::unordered_map<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<std::unordered_map<T...>, TKeySorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<THashMap<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<THashMap<T...>, TKeySorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<std::unordered_multimap<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<std::unordered_map<T...>, TKeyValueSorterComparer<C>> TSorter;
};

template <class C, class... T>
struct TSorterSelector<THashMultiMap<T...>, C, TSortedTag>
{
    typedef TCollectionSorter<THashMultiMap<T...>, TKeyValueSorterComparer<C>> TSorter;
};

////////////////////////////////////////////////////////////////////////////////
// Ordered collections

template <
    class TItemSerializer = TDefaultSerializer,
    class TSortTag = TUnsortedTag
>
struct TVectorSerializer
{
    template <class TVectorType, class C>
    static void Save(C& context, const TVectorType& objects)
    {
        TSizeSerializer::Save(context, objects.size());

        typename TSorterSelector<TVectorType, C, TSortTag>::TSorter sorter(objects);
        for (const auto& object : sorter) {
            TItemSerializer::Save(context, object);
        }
    }

    template <class TVectorType, class C>
    static void Load(C& context, TVectorType& objects)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        objects.resize(size);

        SERIALIZATION_DUMP_WRITE(context, "vector[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, objects[index]);
                }
            }
        }
    }
};

template <
    class TItemSerializer = TDefaultSerializer,
    class TSortTag = TUnsortedTag
>
struct TOptionalVectorSerializer
{
    template <class TVectorType, class C>
    static void Save(C& context, const std::unique_ptr<TVectorType>& objects)
    {
        if (objects) {
            TVectorSerializer<TItemSerializer, TSortTag>::Save(context, *objects);
        } else {
            TSizeSerializer::Save(context, 0);
        }
    }

    template <class TVectorType, class C>
    static void Load(C& context, std::unique_ptr<TVectorType>& objects)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        if (size == 0) {
            objects.reset();
            return;
        }

        objects.reset(new TVectorType());
        objects->resize(size);

        SERIALIZATION_DUMP_WRITE(context, "vector[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, (*objects)[index]);
                }
            }
        }
    }
};

template <class TItemSerializer = TDefaultSerializer>
struct TListSerializer
{
    template <class TListType, class C>
    static void Save(C& context, const TListType& objects)
    {
        TSizeSerializer::Save(context, objects.size());

        for (const auto& object : objects) {
            TItemSerializer::Save(context, object);
        }
    }

    template <class TListType, class C>
    static void Load(C& context, TListType& objects)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        objects.clear();

        SERIALIZATION_DUMP_WRITE(context, "list[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                typename TListType::value_type obj;
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, obj);
                }
                objects.push_back(obj);
            }
        }
    }
};

template <class TItemSerializer = TDefaultSerializer>
struct TArraySerializer
{
    template <class TArray, class C>
    static void Save(C& context, const TArray& objects)
    {
        TSizeSerializer::Save(context, objects.size());

        for (const auto& object : objects) {
            TItemSerializer::Save(context, object);
        }
    }

    template <
        class C,
        class T,
        std::size_t N,
        template <typename U, std::size_t S> class TArray
    >
    static void Load(C& context, TArray<T,N>& objects)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);
        YCHECK(size <= N);

        SERIALIZATION_DUMP_WRITE(context, "array[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, objects[index]);
                }
            }
        }
    }
};

template <class TItemSerializer = TDefaultSerializer>
struct TOptionalListSerializer
{
    template <class TListType, class C>
    static void Save(C& context, const std::unique_ptr<TListType>& objects)
    {
        using NYT::Save;
        if (objects) {
            TListSerializer<TItemSerializer>::Save(context, *objects);
        } else {
            TSizeSerializer::Save(context, 0);
        }
    }

    template <class TListType, class C>
    static void Load(C& context, std::unique_ptr<TListType>& objects)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);

        SERIALIZATION_DUMP_WRITE(context, "list[%v]", size);

        if (size == 0) {
            objects.reset();
            return;
        }

        objects.reset(new TListType());

        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                typename TListType::value_type obj;
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, obj);
                }
                objects->push_back(obj);
            }
        }
    }
};

template <class TItemSerializer = TDefaultSerializer>
struct TEnumIndexedVectorSerializer
{
    template <class T, class E, class C, E Min, E Max>
    static void Save(C& context, const TEnumIndexedVector<T, E, Min, Max>& vector)
    {
        using NYT::Save;

        auto keys = TEnumTraits<E>::GetDomainValues();
        size_t count = 0;
        for (auto key : keys) {
            if (!vector.IsDomainValue(key)) {
                continue;
            }
            ++count;
        }

        TSizeSerializer::Save(context, count);

        for (auto key : keys) {
            if (!vector.IsDomainValue(key)) {
                continue;
            }
            Save(context, key);
            TItemSerializer::Save(context, vector[key]);
        }
    }

    template <class T, class E, class C, E Min, E Max>
    static void Load(C& context, TEnumIndexedVector<T, E, Min, Max>& vector)
    {
        std::fill(vector.begin(), vector.end(), T());

        size_t size = TSizeSerializer::LoadSuspended(context);

        SERIALIZATION_DUMP_WRITE(context, "vector[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != size; ++index) {
                auto key = LoadSuspended<E>(context);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", key);
                SERIALIZATION_DUMP_INDENT(context) {
                    if (!vector.IsDomainValue(key)) {
                        T dummy;
                        TItemSerializer::Load(context, dummy);
                    } else {
                        TItemSerializer::Load(context, vector[key]);
                    }
                }
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////
// Possibly unordered collections

template <
    class TItemSerializer = TDefaultSerializer,
    class TSortTag = TSortedTag
>
struct TSetSerializer
{
    template <class TSetType, class C>
    static void Save(C& context, const TSetType& set)
    {
        TSizeSerializer::Save(context, set.size());

        typename TSorterSelector<TSetType, C, TSortTag>::TSorter sorter(set);
        for (const auto& item : sorter) {
            TItemSerializer::Save(context, item);
        }
    }

    template <class TSetType, class C>
    static void Load(C& context, TSetType& set)
    {
        typedef typename TSetType::key_type TKey;

        size_t size = TSizeSerializer::LoadSuspended(context);

        set.clear();

        SERIALIZATION_DUMP_WRITE(context, "set[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index < size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);

                TKey key;
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, key);
                }

                YCHECK(set.insert(key).second);
            }
        }
    }
};

template <
    class TItemSerializer = TDefaultSerializer,
    class TSortTag = TSortedTag
>
struct TMultiSetSerializer
{
    template <class TSetType, class C>
    static void Save(C& context, const TSetType& set)
    {
        TSetSerializer<TItemSerializer, TSortTag>::Save(context, set);
    }

    template <class TSetType, class C>
    static void Load(C& context, TSetType& set)
    {
        typedef typename TSetType::key_type TKey;

        size_t size = TSizeSerializer::LoadSuspended(context);

        set.clear();

        SERIALIZATION_DUMP_WRITE(context, "multiset[%v]", size);
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index < size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);

                TKey key;
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, key);
                }

                set.insert(key);
            }
        }
    }
};

template <
    class TItemSerializer = TDefaultSerializer,
    class TSortTag = TSortedTag
>
struct TOptionalSetSerializer
{
    template <class TSetType, class C>
    static void Save(C& context, const std::unique_ptr<TSetType>& set)
    {
        if (set) {
            TSetSerializer<TItemSerializer, TSortTag>::Save(context, *set);
        } else {
            TSizeSerializer::Save(context, 0);
        }
    }

    template <class TSetType, class C>
    static void Load(C& context, std::unique_ptr<TSetType>& set)
    {
        typedef typename TSetType::key_type TKey;

        size_t size = TSizeSerializer::LoadSuspended(context);

        SERIALIZATION_DUMP_WRITE(context, "set[%v]", size);

        if (size == 0) {
            set.reset();
            return;
        }

        set.reset(new TSetType());

        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index < size; ++index) {
                SERIALIZATION_DUMP_WRITE(context, "%v =>", index);

                TKey key;
                SERIALIZATION_DUMP_INDENT(context) {
                    TItemSerializer::Load(context, key);
                }

                YCHECK(set->insert(key).second);
            }
        }
    }
};

template <
    class TKeySerializer = TDefaultSerializer,
    class TValueSerializer = TDefaultSerializer,
    class TSortTag = TSortedTag
>
struct TMapSerializer
{
    template <class TMapType, class C>
    static void Save(C& context, const TMapType& map)
    {
        TSizeSerializer::Save(context, map.size());

        typename TSorterSelector<TMapType, C, TSortTag>::TSorter sorter(map);
        for (const auto& pair : sorter) {
            TKeySerializer::Save(context, pair.first);
            TValueSerializer::Save(context, pair.second);
        }
    }

    template <class TMapType, class C>
    static void Load(C& context, TMapType& map)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);

        SERIALIZATION_DUMP_WRITE(context, "map[%v]", size);

        map.clear();

        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index < size; ++index) {
                typename TMapType::key_type key;
                TKeySerializer::Load(context, key);

                SERIALIZATION_DUMP_WRITE(context, "=>");

                typename TMapType::mapped_type value;
                SERIALIZATION_DUMP_INDENT(context) {
                    TValueSerializer::Load(context, value);
                }

                YCHECK(map.emplace(key, std::move(value)).second);
            }
        }
    }
};

template <
    class TKeySerializer = TDefaultSerializer,
    class TValueSerializer = TDefaultSerializer,
    class TSortTag = TSortedTag
>
struct TMultiMapSerializer
{
    template <class TMapType, class C>
    static void Save(C& context, const TMapType& map)
    {
        TMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TSortTag
        >::Save(context, map);
    }

    template <class TMapType, class C>
    static void Load(C& context, TMapType& map)
    {
        size_t size = TSizeSerializer::LoadSuspended(context);

        SERIALIZATION_DUMP_WRITE(context, "multimap[%v]", size);

        map.clear();

        for (size_t index = 0; index < size; ++index) {
            typename TMapType::key_type key;
            TKeySerializer::Load(context, key);

            SERIALIZATION_DUMP_WRITE(context, "=>");

            typename TMapType::mapped_type value;
            SERIALIZATION_DUMP_INDENT(context) {
                TValueSerializer::Load(context, value);
            }

            map.insert(std::make_pair(key, value));
        }
    }
};

template <class T, size_t Size = std::tuple_size<T>::value>
struct TTupleSerializer;

template <class T>
struct TTupleSerializer<T, 0U>
{
    template<class C>
    static void Save(C&, const T&) {}

    template<class C>
    static void Load(C&, T&) {}
};

template <class T, size_t Size>
struct TTupleSerializer
{
    template<class C>
    static void Save(C& context, const T& tuple)
    {
        TTupleSerializer<T, Size - 1U>::Save(context, tuple);
        NYT::Save(context, std::get<Size - 1U>(tuple));
    }

    template<class C>
    static void Load(C& context, T& tuple)
    {
        TTupleSerializer<T, Size - 1U>::Load(context, tuple);
        NYT::Load(context, std::get<Size - 1U>(tuple));
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TValueBoundComparer
{
    template <class T>
    static bool Compare(const T& lhs, const T& rhs)
    {
        return lhs < rhs;
    }
};

template <class T, class C, class>
struct TSerializerTraits
{
    typedef TValueBoundSerializer TSerializer;
    typedef TValueBoundComparer TComparer;
};

////////////////////////////////////////////////////////////////////////////////

template <class TUnderlyingSerializer = TDefaultSerializer>
struct TUniquePtrSerializer
{
    template <class T, class C>
    static void Save(C& context, const std::unique_ptr<T>& ptr)
    {
        using NYT::Save;
        if (ptr) {
            Save(context, true);
            TUnderlyingSerializer::Save(context, *ptr);
        } else {
            Save(context, false);
        }
    }

    template <class T, class C>
    static void Load(C& context, std::unique_ptr<T>& ptr)
    {
        if (LoadSuspended<bool>(context)) {
            ptr = std::make_unique<T>();
            TUnderlyingSerializer::Load(context, *ptr);
        } else {
            ptr.reset();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class C>
struct TSerializerTraits<TSharedRef, C, void>
{
    typedef TSharedRefSerializer TSerializer;
};

template <class C>
struct TSerializerTraits<TSharedRefArray, C, void>
{
    typedef TSharedRefArraySerializer TSerializer;
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename std::enable_if<NMpl::TIsPod<T>::Value && !std::is_pointer<T>::value>::type
>
{
    typedef TPodSerializer TSerializer;
    typedef TValueBoundComparer TComparer;
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename TEnumTraits<T>::TType
>
{
    typedef TEnumSerializer TSerializer;
    typedef TValueBoundComparer TComparer;
};

template <class C>
struct TSerializerTraits<TString, C, void>
{
    typedef TStringSerializer TSerializer;
    typedef TValueBoundComparer TComparer;
};

template <class T, class C>
struct TSerializerTraits<std::optional<T>, C, void>
{
    typedef TOptionalSerializer<> TSerializer;
};

template <class... Ts, class C>
struct TSerializerTraits<std::variant<Ts...>, C, void>
{
    typedef TVariantSerializer TSerializer;
};

template <class T, class C>
struct TSerializerTraits<std::atomic<T>, C, void>
{
    typedef TAtomicSerializer<> TSerializer;
};

template <class T, class A, class C>
struct TSerializerTraits<std::vector<T, A>, C, void>
{
    typedef TVectorSerializer<> TSerializer;
};

template <class T, unsigned size, class C>
struct TSerializerTraits<SmallVector<T, size>, C, void>
{
    typedef TVectorSerializer<> TSerializer;
};

template <class T, std::size_t size, class C>
struct TSerializerTraits<std::array<T, size>, C, void>
{
    typedef TArraySerializer<> TSerializer;
};

template <class T, class A, class C>
struct TSerializerTraits<std::list<T, A>, C, void>
{
    typedef TListSerializer<> TSerializer;
};

template <class T, class C>
struct TSerializerTraits<std::deque<T>, C, void>
{
    typedef TListSerializer<> TSerializer;
};

template <class T, class Q, class A, class C>
struct TSerializerTraits<std::set<T, Q, A>, C, void>
{
    typedef TSetSerializer<> TSerializer;
};
template <class T, class H, class P, class A, class C>
struct TSerializerTraits<std::unordered_set<T, H, P, A>, C, void>
{
    typedef TSetSerializer<> TSerializer;
};

template <class T, class H, class E, class A, class C>
struct TSerializerTraits<THashSet<T, H, E, A>, C, void>
{
    typedef TSetSerializer<> TSerializer;
};

template <class T, class C>
struct TSerializerTraits<THashMultiSet<T>, C, void>
{
    typedef TMultiSetSerializer<> TSerializer;
};

template <class T, class A, class C>
struct TSerializerTraits<std::unique_ptr<std::vector<T, A>>, C, void>
{
    typedef TOptionalVectorSerializer<> TSerializer;
};

template <class T, unsigned size, class C>
struct TSerializerTraits<std::unique_ptr<SmallVector<T, size>>, C, void>
{
    typedef TOptionalVectorSerializer<> TSerializer;
};

template <class T, class A, class C>
struct TSerializerTraits<std::unique_ptr<std::list<T, A>>, C, void>
{
    typedef TOptionalListSerializer<> TSerializer;
};

template <class T, class Q, class A, class C>
struct TSerializerTraits<std::unique_ptr<std::set<T, Q, A>>, C, void>
{
    typedef TOptionalSetSerializer<> TSerializer;
};

template <class T, class H, class P, class A, class C>
struct TSerializerTraits<std::unique_ptr<std::unordered_set<T, H, P, A>>, C, void>
{
    typedef TOptionalSetSerializer<> TSerializer;
};

template <class T, class H, class E, class A, class C>
struct TSerializerTraits<std::unique_ptr<THashSet<T, H, E, A>>, C, void>
{
    typedef TOptionalSetSerializer<> TSerializer;
};

template <class K, class V, class Q, class A, class C>
struct TSerializerTraits<std::map<K, V, Q, A>, C, void>
{
    typedef TMapSerializer<> TSerializer;
};

template <class K, class V, class H, class P, class A, class C>
struct TSerializerTraits<std::unordered_map<K, V, H, P, A>, C, void>
{
    typedef TMapSerializer<> TSerializer;
};

template <class K, class V, class Q, class A, class C>
struct TSerializerTraits<THashMap<K, V, Q, A>, C, void>
{
    typedef TMapSerializer<> TSerializer;
};

template <class K, class V, class Q, class A, class C>
struct TSerializerTraits<std::multimap<K, V, Q, A>, C, void>
{
    typedef TMultiMapSerializer<> TSerializer;
};

template <class K, class V, class C>
struct TSerializerTraits<THashMultiMap<K, V>, C, void>
{
    typedef TMultiMapSerializer<> TSerializer;
};

template <class T, class E, class C, E Min, E Max>
struct TSerializerTraits<TEnumIndexedVector<T, E, Min, Max>, C, void>
{
    typedef TEnumIndexedVectorSerializer<> TSerializer;
};

template <class F, class S, class C>
struct TSerializerTraits<std::pair<F, S>, C, void>
{
    typedef TTupleSerializer<std::pair<F, S>> TSerializer;
    typedef TValueBoundComparer TComparer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
