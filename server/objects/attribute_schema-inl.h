#pragma once
#ifndef ATTRIBUTE_SCHEMA_INL_H_
#error "Direct inclusion of this file is not allowed, include attribute_schema.h"
// For the sake of sane code completion.
#include "attribute_schema.h"
#endif

#include "helpers.h"
#include "type_info.h"

#include <yt/core/misc/string.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/tree_visitor.h>
#include <yt/core/ytree/tree_builder.h>

#include <yt/core/yson/protobuf_interop.h>

#include <contrib/libs/protobuf/io/zero_copy_stream_impl_lite.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

namespace {

struct TEmptyPathValidator
{
    static void Run(const TAttributeSchema* attribute, const NYPath::TYPath& path)
    {
        if (!path.empty()) {
            THROW_ERROR_EXCEPTION("Attribute %v is scalar and does not support nested access",
                attribute->GetPath());
        }
    }
};

template <class T, class = void>
struct TScalarAttributePathValidator
{
    static void Run(const TScalarAttributeSchemaBase* schema, const TAttributeSchema* attribute, const NYPath::TYPath& path)
    {
        if (schema->Field->Type != NTableClient::EValueType::Any && !path.empty()) {
            THROW_ERROR_EXCEPTION("Attribute %v is scalar and does not support nested access",
                attribute->GetPath());
        }
    }
};

template <class T>
struct TScalarAttributePathValidator<
    T,
    typename std::enable_if<NMpl::TIsConvertible<T*, ::google::protobuf::MessageLite*>::Value>::type
>
{
    static void Run(const TScalarAttributeSchemaBase* /*schema*/, const TAttributeSchema* attribute, const NYPath::TYPath& path)
    {
        const auto* protobufType = NYson::ReflectProtobufMessageType<T>();
        try {
            // NB: This is a mere validation; the result is ignored intentionally.
            NYson::ResolveProtobufElementByYPath(protobufType, path);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error fetching field %v of attribute %v",
                path,
                attribute->GetPath())
                << ex;
        }
    }
};

} // namespace

template <class TTypedObject, class TTypedValue>
TAttributeSchema* TAttributeSchema::SetSetter(std::function<void(
    TTransaction*,
    TTypedObject*,
    const NYT::NYPath::TYPath&,
    const TTypedValue&,
    bool recurisve)> setter)
{
    Setter_ =
        [=] (
            TTransaction* transaction,
            TObject* object,
            const NYT::NYPath::TYPath& path,
            const NYT::NYTree::INodePtr& value,
            bool recursive)
        {
            auto typedValue = NYTree::ConvertTo<TTypedValue>(value);
            auto* typedObject = object->template As<TTypedObject>();
            setter(transaction, typedObject, path, typedValue, recursive);
        };

    return this;
}

template <class TTypedObject, class TTypedValue>
TAttributeSchema* TAttributeSchema::SetControl(std::function<void(
    TTransaction*,
    TTypedObject*,
    const TTypedValue&)> control)
{
    Updatable_ = true;
    SetSetter<TTypedObject, TTypedValue>([=, control = std::move(control)] (
        TTransaction* transaction,
        TTypedObject* object,
        const NYT::NYPath::TYPath& path,
        const TTypedValue& value,
        bool /*recursive*/) {
            if (!path.empty()) {
                THROW_ERROR_EXCEPTION("Partial updates are not supported");
            }
            control(transaction, object, value);
        });
    return this;
}

template <class TTypedObject>
TAttributeSchema* TAttributeSchema::SetUpdatePrehandler(std::function<void(
    TTransaction*,
    TTypedObject*)> prehandler)
{
    UpdatePrehandlers_.push_back(
        [=] (TTransaction* transaction, TObject* object) {
            auto* typedObject = object->template As<TTypedObject>();
            prehandler(transaction, typedObject);
        });
    return this;
}

template <class TTypedObject>
TAttributeSchema* TAttributeSchema::SetUpdateHandler(std::function<void(
    TTransaction*,
    TTypedObject*)> handler)
{
    UpdateHandlers_.push_back(
        [=] (TTransaction* transaction, TObject* object) {
            auto* typedObject = object->template As<TTypedObject>();
            handler(transaction, typedObject);
        });
    return this;
}

template <class TTypedObject>
TAttributeSchema* TAttributeSchema::SetValidator(std::function<void(
    TTransaction*,
    TTypedObject*)> handler)
{
    Validators_.push_back(
        [=] (TTransaction* transaction, TObject* object) {
            auto* typedObject = object->template As<TTypedObject>();
            handler(transaction, typedObject);
        });
    return this;
}

template <class TTypedObject>
TAttributeSchema* TAttributeSchema::SetPreevaluator(std::function<void(
    TTransaction*,
    TTypedObject*)> preevaluator)
{
    Preevaluator_ =
        [=] (TTransaction* transaction, TObject* object) {
            auto* typedObject = object->template As<TTypedObject>();
            preevaluator(transaction, typedObject);
        };
    return this;
}

template <class TTypedObject>
TAttributeSchema* TAttributeSchema::SetEvaluator(std::function<void(
    TTransaction*,
    TTypedObject*,
    NYson::IYsonConsumer*)> evaluator)
{
    Evaluator_ =
        [=] (TTransaction* transaction, TObject* object, NYson::IYsonConsumer* consumer) {
            auto* typedObject = object->template As<TTypedObject>();
            evaluator(transaction, typedObject, consumer);
        };
    return this;
}

template <class TTypedObject, class TTypedValue>
TAttributeSchema* TAttributeSchema::SetAttribute(const TScalarAttributeSchema<TTypedObject, TTypedValue>& schema)
{
    InitSetter<TTypedObject, TTypedValue>(schema);
    InitInitializer<TTypedObject, TTypedValue>(schema);
    InitRemover<TTypedObject, TTypedValue>(schema);
    InitPreloader<TTypedObject>(schema);
    InitExpressionBuilder(
        schema.Field,
        std::bind(TScalarAttributePathValidator<TTypedValue>::Run, &schema, std::placeholders::_1, std::placeholders::_2));
    return this;
}

template <class TOne, class TMany>
TAttributeSchema* TAttributeSchema::SetAttribute(const TManyToOneAttributeSchema<TMany, TOne>& schema)
{
    Setter_ =
        [=] (
            TTransaction* transaction,
            TObject* many,
            const NYT::NYPath::TYPath& path,
            const NYT::NYTree::INodePtr& value,
            bool /*recursive*/)
        {
            if (!path.empty()) {
                THROW_ERROR_EXCEPTION("Partial updates are not supported");
            }

            TObjectId id;
            try {
                id = NYTree::ConvertTo<TObjectId>(value);
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION(
                    NClient::NApi::EErrorCode::InvalidObjectId,
                    "Error parsing object id %Qv")
                    << ex;
            }

            auto* typedMany = many->As<TMany>();
            if (id) {
                auto* one = transaction->GetObject(TOne::Type, id);
                one->ValidateExists();
                auto* typedOne = one->template As<TOne>();
                auto* inverseAttribute = schema.InverseAttributeGetter(typedOne);
                inverseAttribute->Add(typedMany);
            } else {
                if (!schema.Nullable) {
                    THROW_ERROR_EXCEPTION("Cannot set null %v",
                        GetHumanReadableTypeName(TOne::Type));
                }
                auto* forwardAttribute = schema.ForwardAttributeGetter(typedMany);
                auto* currentTypedOne = forwardAttribute->Load();
                if (currentTypedOne) {
                    auto* inverseAttribute = schema.InverseAttributeGetter(currentTypedOne);
                    inverseAttribute->Remove(typedMany);
                }
            }
        };

    Remover_ =
        [=] (
            TTransaction* /*transaction*/,
            TObject* many,
            const NYT::NYPath::TYPath& path)
        {
            if (!path.empty()) {
                THROW_ERROR_EXCEPTION("Partial removes are not supported");
            }

            if (!schema.Nullable) {
                THROW_ERROR_EXCEPTION("Cannot set null %v",
                    GetHumanReadableTypeName(TOne::Type));
            }

            auto* typedMany = many->As<TMany>();
            auto* forwardAttribute = schema.ForwardAttributeGetter(typedMany);
            auto* currentTypedOne = forwardAttribute->Load();
            if (currentTypedOne) {
                auto* inverseAttribute = schema.InverseAttributeGetter(currentTypedOne);
                inverseAttribute->Remove(typedMany);
            }
        };

    InitExpressionBuilder(
        schema.Field,
        TEmptyPathValidator::Run);

    return this;
}

template <class TTypedObject, class TTypedValue>
TAttributeSchema* TAttributeSchema::SetProtobufEvaluator(const TScalarAttributeSchema<TTypedObject, TString>& schema)
{
    SetPreevaluator<TTypedObject>([=] (TTransaction* /*transaction*/, TTypedObject* object) {
        auto* attribute = schema.AttributeGetter(object);
        attribute->ScheduleLoad();
    });

    SetEvaluator<TTypedObject>([=] (TTransaction* /*transaction*/, TTypedObject* object, NYson::IYsonConsumer* consumer) {
        auto* attribute = schema.AttributeGetter(object);
        auto protobuf = attribute->Load();
        google::protobuf::io::ArrayInputStream inputStream(protobuf.data(), protobuf.length());
        NYson::ParseProtobuf(
            consumer,
            &inputStream,
            NYson::ReflectProtobufMessageType<TTypedValue>());
    });

    return this;
}

template <class TTypedObject, class TTypedValue>
TAttributeSchema* TAttributeSchema::SetProtobufSetter(const TScalarAttributeSchema<TTypedObject, TString>& schema)
{
    Updatable_ = true;

    Setter_ =
        [=] (
            TTransaction* transaction,
            TObject* object,
            const NYT::NYPath::TYPath& path,
            const NYT::NYTree::INodePtr& value,
            bool recursive)
        {
            auto* typedObject = object->template As<TTypedObject>();
            auto* attribute = schema.AttributeGetter(typedObject);
            TString protobuf;
            if (path.empty()) {
                google::protobuf::io::StringOutputStream outputStream(&protobuf);
                auto protobufWriter = NYson::CreateProtobufWriter(
                    &outputStream,
                    NYson::ReflectProtobufMessageType<TTypedValue>());
                NYTree::VisitTree(value, protobufWriter.get(), true);
            } else {
                // TODO(babenko): optimize
                auto oldProtobuf = attribute->Load();
                google::protobuf::io::ArrayInputStream inputStream(oldProtobuf.data(), oldProtobuf.length());
                auto treeBuilder = NYTree::CreateBuilderFromFactory(NYTree::GetEphemeralNodeFactory());
                NYson::ParseProtobuf(
                    treeBuilder.get(),
                    &inputStream,
                    NYson::ReflectProtobufMessageType<TTypedValue>());

                auto node = treeBuilder->EndTree();
                NYT::NYTree::SyncYPathSet(node, path, NYT::NYTree::ConvertToYsonString(value), recursive);

                google::protobuf::io::StringOutputStream outputStream(&protobuf);
                auto protobufWriter = NYson::CreateProtobufWriter(
                    &outputStream,
                    NYson::ReflectProtobufMessageType<TTypedValue>());
                NYTree::VisitTree(node, protobufWriter.get(), true);
            }
            if (object->GetState() == EObjectState::Creating && schema.Initializer) {
                schema.Initializer(transaction, typedObject, &protobuf);
            }
            if (schema.OldNewValueValidator) {
                schema.OldNewValueValidator(transaction, typedObject, attribute->Load(), protobuf);
            }
            if (schema.NewValueValidator) {
                schema.NewValueValidator(transaction, typedObject, protobuf);
            }
            attribute->Store(std::move(protobuf));
        };

    return this;
}

template <class TTypedObject, class TSchema>
void TAttributeSchema::InitPreloader(const TSchema& schema)
{
    Preloader_ =
        [=] (
            TTransaction* /*transaction*/,
            TObject* object,
            const TUpdateRequest& /*request*/)
        {
            auto* typedObject = object->template As<TTypedObject>();
            auto* attribute = schema.AttributeGetter(typedObject);
            attribute->ScheduleLoad();
        };
}

template <class T>
struct TAttributeValidatorTraits
{
    static void Run(const void*, const void*, const void*)
    { }
};

template <class TTypedObject, class TTypedValue>
struct TAttributeValidatorTraits<TScalarAttributeSchema<TTypedObject, TTypedValue>>
{
    static void Run(
        TTransaction* transaction,
        TTypedObject* typedObject,
        const TScalarAttributeSchema<TTypedObject, TTypedValue>& schema,
        TScalarAttribute<TTypedValue>* attribute,
        TTypedValue* value)
    {
        if (schema.OldNewValueValidator) {
            schema.OldNewValueValidator(transaction, typedObject, attribute->Load(), *value);
        }
        if (schema.NewValueValidator) {
            schema.NewValueValidator(transaction, typedObject, *value);
        }
    }
};

template <class TTypedObject, class TTypedValue, class TSchema>
void TAttributeSchema::InitSetter(const TSchema& schema)
{
    Setter_ =
        [=] (
            TTransaction* transaction,
            TObject* object,
            const NYT::NYPath::TYPath& path,
            const NYT::NYTree::INodePtr& value,
            bool recursive)
        {
            auto* typedObject = object->template As<TTypedObject>();
            auto* attribute = schema.AttributeGetter(typedObject);

            NYT::NYTree::INodePtr newValue;
            if (path.empty()) {
                newValue = value;
            } else {
                // TODO(babenko): optimize
                auto existingValue = NYT::NYTree::ConvertToNode(attribute->Load());
                NYT::NYTree::SyncYPathSet(existingValue, path, NYT::NYTree::ConvertToYsonString(value), recursive);
                newValue = existingValue;
            }

            auto typedValue = NYTree::ConvertTo<TTypedValue>(newValue);
            if (object->GetState() == EObjectState::Creating && schema.Initializer) {
                schema.Initializer(transaction, typedObject, &typedValue);
            }
            TAttributeValidatorTraits<TSchema>::Run(transaction, typedObject, schema, attribute, &typedValue);
            attribute->Store(typedValue);
        };
}

template <class TTypedObject, class TTypedValue, class TSchema>
void TAttributeSchema::InitInitializer(const TSchema& schema)
{
    if (!schema.Initializer) {
        return;
    }
    Initializer_ =
        [=] (
            TTransaction* transaction,
            TObject* object)
        {
            auto* typedObject = object->template As<TTypedObject>();
            auto* attribute = schema.AttributeGetter(typedObject);
            TTypedValue typedValue{};
            schema.Initializer(transaction, typedObject, &typedValue);
            TAttributeValidatorTraits<TSchema>::Run(transaction, typedObject, schema, attribute, &typedValue);
            attribute->Store(typedValue);
        };
}

template <class TTypedObject, class TTypedValue, class TSchema>
void TAttributeSchema::InitRemover(const TSchema& schema)
{
    Remover_ =
        [=] (
            TTransaction* transaction,
            TObject* object,
            const NYT::NYPath::TYPath& path)
        {
            if (path.empty()) {
                THROW_ERROR_EXCEPTION("Attribute %v cannot be removed",
                    GetPath());
            }

            auto* typedObject = object->template As<TTypedObject>();
            auto* attribute = schema.AttributeGetter(typedObject);

            // TODO(babenko): optimize
            auto existingValue = NYT::NYTree::ConvertToNode(attribute->Load());
            NYT::NYTree::SyncYPathRemove(existingValue, path);
            auto newValue = existingValue;

            auto typedValue = NYTree::ConvertTo<TTypedValue>(newValue);
            TAttributeValidatorTraits<TSchema>::Run(transaction, typedObject, schema, attribute, &typedValue);
            attribute->Store(typedValue);
        };
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
