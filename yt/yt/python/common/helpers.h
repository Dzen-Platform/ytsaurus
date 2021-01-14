#pragma once

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/misc/optional.h>
#include <yt/core/misc/ref.h>

#include <yt/core/ytree/public.h>

#include <util/generic/noncopyable.h>
#include <util/generic/strbuf.h>
#include <util/generic/string.h>

#include <Objects.hxx> // pycxx

namespace Py {

////////////////////////////////////////////////////////////////////////////////
// Extend PyCxx with some useful functions.

static_assert(sizeof(PY_LONG_LONG) == sizeof(i64), "Py_LONG_LONG size does not equal i64 size");

bool IsInteger(const Object& obj);
bool IsFloat(const Object& obj);

#ifdef PYCXX_PYTHON_2TO3
TStringBuf ConvertToStringBuf(PyObject* pyString);
TStringBuf ConvertToStringBuf(const Bytes& pyString);
Bytes ConvertToPythonString(const TString& string);
#endif

TString ConvertStringObjectToString(const Object& obj);
Object GetAttr(const Object& obj, const std::string& fieldName);
i64 ConvertToLongLong(const Object& obj);
std::string Repr(const Object& obj);
TString Str(const Object& obj);

Object CreateIterator(const Object& object);

NYT::TError BuildErrorFromPythonException();

////////////////////////////////////////////////////////////////////////////////

} // namespace Py

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

Py::Object ExtractArgument(Py::Tuple& args, Py::Dict& kwargs, const std::string& name);
bool HasArgument(const Py::Tuple& args, const Py::Dict& kwargs, const std::string& name);
void ValidateArgumentsEmpty(const Py::Tuple& args, const Py::Dict& kwargs);

////////////////////////////////////////////////////////////////////////////////

class TGilGuard
    : private TNonCopyable
{
public:
    TGilGuard();
    ~TGilGuard();

private:
    PyGILState_STATE State_;
    size_t ThreadId_;
};

////////////////////////////////////////////////////////////////////////////////

class TReleaseAcquireGilGuard
    : private TNonCopyable
{
public:
    TReleaseAcquireGilGuard();
    ~TReleaseAcquireGilGuard();

private:
    PyThreadState* State_;
    size_t ThreadId_;
};

////////////////////////////////////////////////////////////////////////////////

class TPythonClassObject
{
public:
    TPythonClassObject();
    explicit TPythonClassObject(PyTypeObject* typeObject);
    Py::Callable Get();

private:
    Py::Callable ClassObject_;
};

////////////////////////////////////////////////////////////////////////////////

PyObject* GetYsonTypeClass(const std::string& name);
PyObject* FindYsonTypeClass(const std::string& name);

////////////////////////////////////////////////////////////////////////////////

bool WaitForSettingFuture(TFuture<void> future);

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
