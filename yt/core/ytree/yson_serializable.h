#pragma once

#include "public.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/mpl.h>
#include <yt/core/misc/nullable.h>
#include <yt/core/misc/property.h>

#include <yt/core/yson/public.h>

#include <functional>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMergeStrategy,
    (Default)
    (Overwrite)
    (Combine)
);

////////////////////////////////////////////////////////////////////////////////

class TYsonSerializableLite
    : private TNonCopyable
{
public:
    typedef std::function<void()> TValidator;
    typedef std::function<void()> TInitializer;

    struct IParameter
        : public TIntrinsicRefCounted
    {
        virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path) = 0;
        virtual void Validate(const NYPath::TYPath& path) const = 0;virtual void SetDefaults() = 0;
        virtual void Save(NYson::IYsonConsumer* consumer) const = 0;
        virtual bool HasValue() const = 0;
        virtual const std::vector<TString>& GetAliases() const = 0;
    };

    typedef TIntrusivePtr<IParameter> IParameterPtr;

    template <class T>
    class TParameter
        : public IParameter
    {
    public:
        typedef std::function<void(const T&)> TValidator;
        typedef typename TNullableTraits<T>::TValueType TValueType;

        explicit TParameter(T& parameter);

        virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path) override;
        virtual void Validate(const NYPath::TYPath& path) const override;
        virtual void SetDefaults() override;
        virtual void Save(NYson::IYsonConsumer* consumer) const override;
        virtual bool HasValue() const override;
        virtual const std::vector<TString>& GetAliases() const override;

    public:
        TParameter& Describe(const char* description);
        TParameter& Optional();
        TParameter& Default(const T& defaultValue = T());
        TParameter& DefaultNew();
        TParameter& CheckThat(TValidator validator);
        TParameter& GreaterThan(TValueType value);
        TParameter& GreaterThanOrEqual(TValueType value);
        TParameter& LessThan(TValueType value);
        TParameter& LessThanOrEqual(TValueType value);
        TParameter& InRange(TValueType lowerBound, TValueType upperBound);
        TParameter& NonEmpty();
        TParameter& Alias(const TString& name);
        TParameter& MergeBy(EMergeStrategy strategy);

    private:
        T& Parameter;
        const char* Description;
        TNullable<T> DefaultValue;
        std::vector<TValidator> Validators;
        std::vector<TString> Aliases;
        EMergeStrategy MergeStrategy;
    };

    TYsonSerializableLite();

    void Load(
        NYTree::INodePtr node,
        bool validate = true,
        bool setDefaults = true,
        const NYPath::TYPath& path = "");

    void Validate(const NYPath::TYPath& path = "") const;

    void SetDefaults();

    void Save(
        NYson::IYsonConsumer* consumer,
        bool stable = false) const;

    DEFINE_BYVAL_RW_PROPERTY(bool, KeepOptions);
    NYTree::IMapNodePtr GetOptions() const;

    yhash_set<TString> GetRegisteredKeys() const;

protected:
    virtual void OnLoaded();

    template <class T>
    TParameter<T>& RegisterParameter(
        const TString& parameterName,
        T& value);

    void RegisterInitializer(const TInitializer& func);
    void RegisterValidator(const TValidator& func);

private:
    template <class T>
    friend class TParameter;

    yhash<TString, IParameterPtr> Parameters;

    NYTree::IMapNodePtr Options;

    std::vector<TInitializer> Initializers;
    std::vector<TValidator> Validators;

};

////////////////////////////////////////////////////////////////////////////////

class TYsonSerializable
    : public TRefCounted
    , public TYsonSerializableLite
{ };

////////////////////////////////////////////////////////////////////////////////

template <class T>
TIntrusivePtr<T> CloneYsonSerializable(TIntrusivePtr<T> obj);

void Serialize(const TYsonSerializableLite& value, NYson::IYsonConsumer* consumer);
void Deserialize(TYsonSerializableLite& value, NYTree::INodePtr node);

NYson::TYsonString ConvertToYsonStringStable(const TYsonSerializableLite& value);

template <class T>
TIntrusivePtr<T> UpdateYsonSerializable(
    TIntrusivePtr<T> obj,
    NYTree::INodePtr patch);

template <class T>
TIntrusivePtr<T> UpdateYsonSerializable(
    TIntrusivePtr<T> obj,
    const NYson::TYsonString& patch);

template <class T>
bool ReconfigureYsonSerializable(
    TIntrusivePtr<T> config,
    const NYson::TYsonString& newConfigYson);

template <class T>
bool ReconfigureYsonSerializable(
    TIntrusivePtr<T> config,
    NYTree::INodePtr newConfigNode);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT


namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TBinaryYsonSerializer
{
    static void Save(TStreamSaveContext& context, const NYTree::TYsonSerializableLite& obj);
    static void Load(TStreamLoadContext& context, NYTree::TYsonSerializableLite& obj);
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, NYTree::TYsonSerializableLite&>>::TType>
{
    typedef TBinaryYsonSerializer TSerializer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define YSON_SERIALIZABLE_INL_H_
#include "yson_serializable-inl.h"
#undef YSON_SERIALIZABLE_INL_H_
