#include "converter_common.h"

#include <yt/yt/python/common/helpers.h>

#include <yt/yt/python/yson/serialize.h>
#include <yt/yt/python/yson/pull_object_builder.h>

#include <yt/yt/core/misc/error.h>
#include <yt/yt/core/misc/enum.h>

#include <yt/yt/core/ytree/convert.h>

#include <util/string/builder.h>

#include <Extensions.hxx> // pycxx
#include <Objects.hxx> // pycxx


namespace NYT::NPython {

using namespace NSkiff;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

bool IsTiTypeOptional(Py::Object pySchema)
{
    return GetAttr(pySchema, IsTiTypeOptionalFieldName).as_bool();
}

PyObjectPtr GetSchemaType(const TString& name)
{
    return PyObjectPtr(GetModuleAttribute("yt.wrapper.schema.internal_schema", name));
}

EPythonType GetPythonType(Py::Object pyType)
{
    if (pyType.is(reinterpret_cast<PyObject*>(&PyLong_Type))) {
        return EPythonType::Int;
    } else if (pyType.is(reinterpret_cast<PyObject*>(&PyUnicode_Type))) {
        return EPythonType::Str;
    } else if (pyType.is(reinterpret_cast<PyObject*>(&PyBytes_Type))) {
        return EPythonType::Bytes;
    } else if (pyType.is(reinterpret_cast<PyObject*>(&PyFloat_Type))) {
        return EPythonType::Float;
    } else if (pyType.is(reinterpret_cast<PyObject*>(&PyBool_Type))) {
        return EPythonType::Bool;
    } else {
        THROW_ERROR_EXCEPTION("It's a bug, please contact yt@. Unexpected python type %Qlv",
            pyType.repr().as_string());
    }
}

TString GetRowClassName(Py::Object pySchema)
{
    auto pyType = GetAttr(GetAttr(pySchema, StructSchemaFieldName), PyTypeFieldName);
    return TString(GetAttr(pyType, QualNameFieldName).as_string());
}

////////////////////////////////////////////////////////////////////////////////

TSkiffOtherColumns::TSkiffOtherColumns(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
    : PythonClass(self, args, kwargs)
{
    if (args.size() > 1) {
        throw Py::TypeError("TSkiffOtherColumns.__init__ takes exactly 1 argument");
    }
    if (args.size() == 0) {
        // Object is in uninitialized state.
        return;
    }
    auto arg = args.getItem(0);
    arg.increment_reference_count();
    if (PyBytes_Check(arg.ptr())) {
        UnparsedBytesObj_ = Py::Bytes(arg);
    } else if (arg.isMapping()) {
        Map_ = arg;
    } else {
        throw Py::TypeError("TSkiffOtherColumns.__init__ argument must have type \"bytes\" or be a mapping");
    }
}

int TSkiffOtherColumns::mapping_length()
{
    MaybeMaterializeMap();
    return Map_->length();
}

Py::Object TSkiffOtherColumns::mapping_subscript(const Py::Object& key)
{
    MaybeMaterializeMap();
    return Map_->getItem(key);
}

int TSkiffOtherColumns::mapping_ass_subscript(const Py::Object& key, const Py::Object& value)
{
    MaybeMaterializeMap();
    Map_->setItem(key, value);
    return 0;
}

Py::Object TSkiffOtherColumns::repr()
{
    MaybeMaterializeMap();
    return Map_->str();
}

Py::Object TSkiffOtherColumns::DeepCopy(const Py::Tuple& /*args*/)
{
    Py::Callable classType(TSkiffOtherColumns::type());
    return classType.apply(Py::TupleN(Py::ConvertToPythonString(GetYsonString().AsStringBuf())), Py::Dict());
}

TStringBuf TSkiffOtherColumns::GetUnparsedBytes() const
{
    char* buffer;
    Py_ssize_t size;
    Y_VERIFY(UnparsedBytesObj_);
    if (PyBytes_AsStringAndSize(UnparsedBytesObj_->ptr(), &buffer, &size) == -1) {
        throw Py::Exception();
    }
    return TStringBuf(buffer, size);
}

TYsonStringBuf TSkiffOtherColumns::GetYsonString()
{
    if (UnparsedBytesObj_) {
        return TYsonStringBuf(GetUnparsedBytes(), EYsonType::Node);
    }
    if (!Map_) {
        throw Py::RuntimeError("TSkiffOtherColumns is unitilialized, GetYsonString should not be called");
    }
    CachedYsonString_ = ConvertToYsonString(Py::Object(*Map_));
    return CachedYsonString_;
}

void TSkiffOtherColumns::InitType()
{
    behaviors().name("yson_lib.SkiffOtherColumns");
    behaviors().doc("Very lazy dict of skiff $other_columns");
    behaviors().supportGetattro();
    behaviors().supportSetattro();
    behaviors().supportMappingType(
        behaviors().support_mapping_ass_subscript |
        behaviors().support_mapping_subscript |
        behaviors().support_mapping_length);
    behaviors().supportRepr();
    behaviors().supportCompare();

    PYCXX_ADD_VARARGS_METHOD(__deepcopy__, DeepCopy, "Deepcopy");

    behaviors().readyType();
}

void TSkiffOtherColumns::MaybeMaterializeMap()
{
    if (Map_) {
        return;
    }
    if (!UnparsedBytesObj_) {
        Map_ = Py::Dict();
        return;
    }
    
    TMemoryInput input(GetUnparsedBytes());
    try {
        TYsonPullParser parser(&input, EYsonType::Node);
        TPullObjectBuilder builder(&parser, /* alwaysCreateAttributes */ false, DefaultEncoding);
        Map_ = Py::Mapping(Py::Object(builder.ParseObjectLazy().release(), /* owned */ true));
    } catch (const std::exception& exception) {
        throw Py::RuntimeError(::TStringBuilder() << "Failed to parse lazy yson: " << exception.what());
    }
    UnparsedBytesObj_.reset();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
