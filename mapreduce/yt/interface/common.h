#pragma once

#include "fwd.h"
#include <mapreduce/yt/node/node.h>

#include <util/generic/guid.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/type_name.h>
#include <util/generic/vector.h>

#include <initializer_list>
#include <type_traits>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

#define FLUENT_FIELD(type, name) \
    type name##_; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_FIELD_OPTION(type, name) \
    TMaybe<type> name##_; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_FIELD_DEFAULT(type, name, defaultValue) \
    type name##_ = defaultValue; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_VECTOR_FIELD(type, name) \
    yvector<type> name##s_; \
    TSelf& Add##name(const type& value) \
    { \
        name##s_.push_back(value); \
        return static_cast<TSelf&>(*this);\
    }

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TKeyBase
{
    TKeyBase()
    { }

    TKeyBase(const TKeyBase& rhs)
    {
        Parts_ = rhs.Parts_;
    }

    TKeyBase& operator=(const TKeyBase& rhs)
    {
        Parts_ = rhs.Parts_;
        return *this;
    }

    TKeyBase(TKeyBase&& rhs)
    {
        Parts_ = std::move(rhs.Parts_);
    }

    TKeyBase& operator=(TKeyBase&& rhs)
    {
        Parts_ = std::move(rhs.Parts_);
        return *this;
    }

    template<class U>
    TKeyBase(std::initializer_list<U> il)
    {
        Parts_.assign(il.begin(), il.end());
    }

    template <class U, class... TArgs, std::enable_if_t<std::is_convertible<U, T>::value, int> = 0>
    TKeyBase(U&& arg, TArgs&&... args)
    {
        Add(arg, std::forward<TArgs>(args)...);
    }

    TKeyBase(yvector<T> args)
        : Parts_(std::move(args))
    { }

    bool operator==(const TKeyBase& rhs) const {
        return Parts_ == rhs.Parts_;
    }

    template <class U, class... TArgs>
    TKeyBase& Add(U&& part, TArgs&&... args) &
    {
        Parts_.push_back(std::forward<U>(part));
        return Add(std::forward<TArgs>(args)...);
    }

    template <class... TArgs>
    TKeyBase Add(TArgs&&... args) &&
    {
        return std::move(Add(std::forward<TArgs>(args)...));
    }

    TKeyBase& Add() &
    {
        return *this;
    }

    TKeyBase Add() &&
    {
        return std::move(*this);
    }

    yvector<T> Parts_;
};

////////////////////////////////////////////////////////////////////////////////

enum EValueType : int
{
    VT_INT64,
    VT_UINT64,
    VT_DOUBLE,
    VT_BOOLEAN,
    VT_STRING,
    VT_ANY
};

enum ESortOrder : int
{
    SO_ASCENDING    /* "ascending" */,
    SO_DESCENDING   /* "descending" */,
};

enum EOptimizeForAttr : i8
{
    OF_SCAN_ATTR    /* "scan" */,
    OF_LOOKUP_ATTR  /* "lookup" */,
};

enum EErasureCodecAttr : i8
{
    EC_NONE_ATTR                /* "none" */,
    EC_REED_SOLOMON_6_3_ATTR    /* "reed_solomon_6_3" */,
    EC_LRC_12_2_2_ATTR          /* "lrc_12_2_2" */,
};

struct TColumnSchema
{
    using TSelf = TColumnSchema;

    FLUENT_FIELD(TString, Name);
    FLUENT_FIELD(EValueType, Type);
    FLUENT_FIELD_OPTION(ESortOrder, SortOrder);
    FLUENT_FIELD_OPTION(TString, Lock);
    FLUENT_FIELD_OPTION(TString, Expression);
    FLUENT_FIELD_OPTION(TString, Aggregate);
    FLUENT_FIELD_OPTION(TString, Group);
};

struct TTableSchema
{
public:
    using TSelf = TTableSchema;

    FLUENT_VECTOR_FIELD(TColumnSchema, Column);
    FLUENT_FIELD_DEFAULT(bool, Strict, true);
    FLUENT_FIELD_DEFAULT(bool, UniqueKeys, false);

public:
    // Some helper methods

    TTableSchema& AddColumn(const TString& name, EValueType type) &;
    TTableSchema AddColumn(const TString& name, EValueType type) &&;

    TTableSchema& AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &;
    TTableSchema AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &&;

    TNode ToNode() const;
};

////////////////////////////////////////////////////////////////////////////////

struct TReadLimit
{
    using TSelf = TReadLimit;

    FLUENT_FIELD_OPTION(TKey, Key);
    FLUENT_FIELD_OPTION(i64, RowIndex);
    FLUENT_FIELD_OPTION(i64, Offset);
};

struct TReadRange
{
    using TSelf = TReadRange;

    FLUENT_FIELD(TReadLimit, LowerLimit);
    FLUENT_FIELD(TReadLimit, UpperLimit);
    FLUENT_FIELD(TReadLimit, Exact);

    static TReadRange FromRowIndexes(i64 lowerLimit, i64 upperLimit)
    {
        return TReadRange()
            .LowerLimit(TReadLimit().RowIndex(lowerLimit))
            .UpperLimit(TReadLimit().RowIndex(upperLimit));
    }
};

struct TRichYPath
{
    using TSelf = TRichYPath;

    FLUENT_FIELD(TYPath, Path);

    FLUENT_FIELD_OPTION(bool, Append);
    FLUENT_FIELD(TKeyColumns, SortedBy);

    FLUENT_VECTOR_FIELD(TReadRange, Range);


    // Specifies columns that should be read.
    // If it's set to Nothing then all columns will be read.
    // If empty TKeyColumns is specified then each read row will be empty.
    FLUENT_FIELD_OPTION(TKeyColumns, Columns);

    FLUENT_FIELD_OPTION(bool, Teleport);
    FLUENT_FIELD_OPTION(bool, Primary);
    FLUENT_FIELD_OPTION(bool, Foreign);
    FLUENT_FIELD_OPTION(i64, RowCountLimit);

    FLUENT_FIELD_OPTION(TString, FileName);
    FLUENT_FIELD_OPTION(bool, Executable);
    FLUENT_FIELD_OPTION(TNode, Format);
    FLUENT_FIELD_OPTION(TTableSchema, Schema);

    FLUENT_FIELD_OPTION(TString, CompressionCodec);
    FLUENT_FIELD_OPTION(EErasureCodecAttr, ErasureCodec);
    FLUENT_FIELD_OPTION(EOptimizeForAttr, OptimizeFor);

    // Timestamp of dynamic table.
    // NOTE: it is _not_ unix timestamp
    // (instead it's transaction timestamp, that is more complex structure).
    FLUENT_FIELD_OPTION(i64, Timestamp);

    TRichYPath()
    { }

    TRichYPath(const char* path)
        : Path_(path)
    { }

    TRichYPath(const TYPath& path)
        : Path_(path)
    { }
};

struct TAttributeFilter
{
    using TSelf = TAttributeFilter;

    FLUENT_VECTOR_FIELD(TString, Attribute);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
