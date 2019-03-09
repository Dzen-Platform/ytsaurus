#include "helpers.h"

#include <yt/core/misc/proc.h>

namespace Py {

////////////////////////////////////////////////////////////////////////////////

bool IsInteger(const Object& obj)
{
#if PY_MAJOR_VERSION < 3
    return PyInt_Check(obj.ptr()) || PyLong_Check(obj.ptr());
#else
    return PyLong_Check(obj.ptr());
#endif
}

bool IsFloat(const Object& obj)
{
    return PyFloat_Check(obj.ptr());
}

TStringBuf ConvertToStringBuf(PyObject* pyString)
{
    char* stringData;
    Py_ssize_t length;
    PyBytes_AsStringAndSize(pyString, &stringData, &length);
    return TStringBuf(stringData, length);
}

TStringBuf ConvertToStringBuf(const Bytes& pyString)
{
    return ConvertToStringBuf(pyString.ptr());
}

TString ConvertStringObjectToString(const Object& obj)
{
    Object pyString = obj;
    if (!PyBytes_Check(pyString.ptr())) {
        if (PyUnicode_Check(pyString.ptr())) {
            pyString = Py::Object(PyUnicode_AsUTF8String(pyString.ptr()), true);
        } else {
            throw RuntimeError("Object '" + Repr(pyString) + "' is not bytes or unicode string");
        }
    }
    char* stringData;
    Py_ssize_t length;
    PyBytes_AsStringAndSize(pyString.ptr(), &stringData, &length);
    return TString(stringData, length);
}

Bytes ConvertToPythonString(const TString& string)
{
    return Py::Bytes(string.c_str(), string.length());
}

i64 ConvertToLongLong(const Object& obj)
{
    return static_cast<i64>(Py::LongLong(obj));
}

Object GetAttr(const Object& obj, const std::string& fieldName)
{
    if (!obj.hasAttr(fieldName)) {
        throw RuntimeError("There is no field " + fieldName);
    }
    return obj.getAttr(fieldName);
}

std::string Repr(const Object& obj)
{
    return obj.repr().as_std_string("utf-8", "replace");
}

Object CreateIterator(const Object& obj)
{
    PyObject* iter = PyObject_GetIter(obj.ptr());
    if (!iter) {
        throw Py::Exception();
    }
    return Object(iter, true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace Py

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

Py::Object ExtractArgument(Py::Tuple& args, Py::Dict& kwargs, const std::string& name)
{
    Py::Object result;
    if (kwargs.hasKey(name)) {
        result = kwargs[name];
        kwargs.delItem(name);
    } else {
        if (args.length() == 0) {
            throw Py::RuntimeError("Missing argument '" + name + "'");
        }
        result = args.front();
        args = args.getSlice(1, args.length());
    }
    return result;
}

bool HasArgument(const Py::Tuple& args, const Py::Dict& kwargs, const std::string& name)
{
    if (kwargs.hasKey(name)) {
        return true;
    } else {
        return args.length() > 0;
    }
}

void ValidateArgumentsEmpty(const Py::Tuple& args, const Py::Dict& kwargs)
{
    if (args.length() > 0) {
        throw Py::RuntimeError("Excessive positinal argument");
    }
    if (kwargs.length() > 0) {
        auto name = ConvertStringObjectToString(kwargs.keys()[0]);
        throw Py::RuntimeError("Excessive named argument '" + name + "'");
    }

}

////////////////////////////////////////////////////////////////////////////////

TGilGuard::TGilGuard()
    : State_(PyGILState_Ensure())
    , ThreadId_(GetCurrentThreadId())
{ }

TGilGuard::~TGilGuard()
{
    YCHECK(ThreadId_ == GetCurrentThreadId());
    PyGILState_Release(State_);
}

////////////////////////////////////////////////////////////////////////////////

TReleaseAcquireGilGuard::TReleaseAcquireGilGuard()
    : State_(PyEval_SaveThread())
    , ThreadId_(GetCurrentThreadId())
{ }

TReleaseAcquireGilGuard::~TReleaseAcquireGilGuard()
{
    YCHECK(ThreadId_ == GetCurrentThreadId());
    PyEval_RestoreThread(State_);
}

////////////////////////////////////////////////////////////////////////////////

TPythonClassObject::TPythonClassObject()
{ }

TPythonClassObject::TPythonClassObject(PyTypeObject* typeObject)
    : ClassObject_(Py::Callable(reinterpret_cast<PyObject*>(typeObject)))
{ }

Py::Callable TPythonClassObject::Get()
{
    return ClassObject_;
}

////////////////////////////////////////////////////////////////////////////////

PyObject* GetYsonTypeClass(const std::string& name)
{
    // TODO(ignat): Make singleton
    static PyObject* ysonTypesModule = PyImport_ImportModuleNoBlock("yt.yson.yson_types");
    if (!ysonTypesModule) {
        throw Py::RuntimeError("Failed to import module yt.yson.yson_types");
    }
    return PyObject_GetAttrString(ysonTypesModule, name.c_str());
}

////////////////////////////////////////////////////////////////////////////////

bool WaitForSettingFuture(TFuture<void> future)
{
    while (true) {
        {
            TGilGuard guard;
            auto signals = PyErr_CheckSignals();
            if (signals == -1) {
                return false;
            }
        }

        auto result = future.TimedWait(TDuration::MilliSeconds(100));
        if (result) {
            return true;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
