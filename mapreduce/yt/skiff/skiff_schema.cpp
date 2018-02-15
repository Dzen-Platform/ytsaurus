#include "skiff_schema.h"

namespace NSkiff {

////////////////////////////////////////////////////////////////////////////////

void PrintShortDebugString(const TSkiffSchemaPtr& schema, IOutputStream* out)
{
    (*out) << ToString(schema->GetWireType());
    if (!IsSimpleType(schema->GetWireType())) {
        auto children = schema->GetChildren();
        if (!children.empty()) {
            (*out) << '<';
            for (const auto& child : children) {
                PrintShortDebugString(child, out);
                (*out) << ';';
            }
            (*out) << '>';
        }
    }
}

TString GetShortDebugString(const TSkiffSchemaPtr& schema)
{
    TStringStream out;
    PrintShortDebugString(schema, &out);
    return out.Str();
}

TSimpleTypeSchemaPtr CreateSimpleTypeSchema(EWireType type)
{
    return ::MakeIntrusive<TSimpleTypeSchema>(type);
}

static void VerifyNonemptyChildren(const TSkiffSchemaList& children, EWireType wireType)
{
    if (children.empty()) {
        ythrow yexception() << wireType << " must have at least one child";
    }
}

TTupleSchemaPtr CreateTupleSchema(TSkiffSchemaList children)
{
    VerifyNonemptyChildren(children, EWireType::Tuple);
    return ::MakeIntrusive<TTupleSchema>(std::move(children));
}

TVariant8SchemaPtr CreateVariant8Schema(TSkiffSchemaList children)
{
    VerifyNonemptyChildren(children, EWireType::Variant8);
    return ::MakeIntrusive<TVariant8Schema>(std::move(children));
}

TVariant16SchemaPtr CreateVariant16Schema(TSkiffSchemaList children)
{
    VerifyNonemptyChildren(children, EWireType::Variant16);
    return ::MakeIntrusive<TVariant16Schema>(std::move(children));
}

TRepeatedVariant16SchemaPtr CreateRepeatedVariant16Schema(TSkiffSchemaList children)
{
    VerifyNonemptyChildren(children, EWireType::RepeatedVariant16);
    return ::MakeIntrusive<TRepeatedVariant16Schema>(std::move(children));
}

////////////////////////////////////////////////////////////////////////////////

TSkiffSchema::TSkiffSchema(EWireType type)
    : Type_(type)
{ }

EWireType TSkiffSchema::GetWireType() const
{
    return Type_;
}

TSkiffSchemaPtr TSkiffSchema::SetName(TString name)
{
    Name_ = std::move(name);
    return this;
}

const TString& TSkiffSchema::GetName() const
{
    return Name_;
}

TSkiffSchemaList TSkiffSchema::GetChildren() const
{
    return {};
}

////////////////////////////////////////////////////////////////////////////////

TSimpleTypeSchema::TSimpleTypeSchema(EWireType type)
    : TSkiffSchema(type)
{
    Y_ENSURE(IsSimpleType(type));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSkiff
