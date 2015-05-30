#include "public.h"
#include "helpers.h"
#include "stream.h"
#include "serialize.h"
#include "shutdown.h"

#include <core/ytree/convert.h>

#include <contrib/libs/pycxx/Objects.hxx>
#include <contrib/libs/pycxx/Extensions.hxx>

namespace NYT {
namespace NPython {

using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

Py::Exception CreateYsonError(const std::string& message)
{
    static PyObject* ysonErrorClass = nullptr;
    if (!ysonErrorClass) {
        ysonErrorClass = PyObject_GetAttr(
            PyImport_ImportModule("yt.yson.common"),
            PyString_FromString("YsonError"));
    }
    return Py::Exception(ysonErrorClass, message);
}

///////////////////////////////////////////////////////////////////////////////

class TYsonIterator
    : public Py::PythonClass<TYsonIterator>
{
public:
    TYsonIterator(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
        : Py::PythonClass<TYsonIterator>::PythonClass(self, args, kwargs)
    { }

    void Init(TInputStream* inputStream, std::unique_ptr<TInputStream> inputStreamOwner, bool alwaysCreateAttributes)
    {
        YCHECK(!inputStreamOwner || inputStreamOwner.get() == inputStream);
        InputStream_ = inputStream;
        InputStreamOwner_ = std::move(inputStreamOwner);
        Consumer_.reset(new NYTree::TPythonObjectBuilder(alwaysCreateAttributes));
        Parser_.reset(new NYson::TYsonParser(Consumer_.get(), NYson::EYsonType::ListFragment));
        IsStreamRead_ = false;

    }

    Py::Object iter()
    {
        return self();
    }

    PyObject* iternext()
    {
        try {
            // Read unless we have whole row
            while (!Consumer_->HasObject() && !IsStreamRead_) {
                int length = InputStream_->Read(Buffer_, BufferSize_);
                if (length != 0) {
                    Parser_->Read(TStringBuf(Buffer_, length));
                }
                if (BufferSize_ != length) {
                    IsStreamRead_ = true;
                    Parser_->Finish();
                }
            }

            // Stop iteration if we done
            if (!Consumer_->HasObject()) {
                PyErr_SetNone(PyExc_StopIteration);
                return 0;
            }

            auto result = Consumer_->ExtractObject();
            // We should return pointer to alive object
            result.increment_reference_count();
            return result.ptr();
        } catch (const std::exception& ex) {
            throw CreateYsonError(ex.what());
        }
    }

    virtual ~TYsonIterator()
    { }

    static void InitType()
    {
        behaviors().name("Yson iterator");
        behaviors().doc("Iterates over stream with YSON rows");
        behaviors().supportGetattro();
        behaviors().supportSetattro();
        behaviors().supportIter();

        behaviors().readyType();
    }

private:
    TInputStream* InputStream_;
    std::unique_ptr<TInputStream> InputStreamOwner_;

    bool IsStreamRead_;

    std::unique_ptr<NYTree::TPythonObjectBuilder> Consumer_;
    std::unique_ptr<NYson::TYsonParser> Parser_;

    static const int BufferSize_ = 1024 * 1024;
    char Buffer_[BufferSize_];
};

///////////////////////////////////////////////////////////////////////////////

class TRawYsonIterator
    : public Py::PythonClass<TRawYsonIterator>
{
public:
    TRawYsonIterator(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
        : Py::PythonClass<TRawYsonIterator>::PythonClass(self, args, kwargs)
    { }

    void Init(TInputStream* inputStream, std::unique_ptr<TInputStream> inputStreamOwner)
    {
        InputStreamOwner_ = std::move(inputStreamOwner);
        Lexer_ = std::move(TListFragmentLexer(inputStream));
    }

    Py::Object iter()
    {
        return self();
    }

    PyObject* iternext()
    {
        try {
            auto item = Lexer_.NextItem();
            if (!item) {
                PyErr_SetNone(PyExc_StopIteration);
                return 0;
            }
            auto result = Py::String(item.Begin(), item.Size());
            result.increment_reference_count();
            return result.ptr();
        } catch (const std::exception& ex) {
            throw CreateYsonError(ex.what());
        }
    }

    virtual ~TRawYsonIterator()
    { }

    static void InitType()
    {
        behaviors().name("Yson iterator");
        behaviors().doc("Iterates over stream with YSON rows");
        behaviors().supportGetattro();
        behaviors().supportSetattro();
        behaviors().supportIter();

        behaviors().readyType();
    }

private:
    std::unique_ptr<TInputStream> InputStreamOwner_;
    TListFragmentLexer Lexer_;
};

///////////////////////////////////////////////////////////////////////////////

class TYsonModule
    : public Py::ExtensionModule<TYsonModule>
{
public:
    TYsonModule()
        // This should match .so file name.
        : Py::ExtensionModule<TYsonModule>("yson_lib")
    {
        PyEval_InitThreads();

        RegisterShutdown();

        TYsonIterator::InitType();
        TRawYsonIterator::InitType();

        add_keyword_method("load", &TYsonModule::Load, "Loads YSON from stream");
        add_keyword_method("loads", &TYsonModule::Loads, "Loads YSON from string");

        add_keyword_method("dump", &TYsonModule::Dump, "Dumps YSON to stream");
        add_keyword_method("dumps", &TYsonModule::Dumps, "Dumps YSON to string");

        initialize("Python bindings for YSON");
    }

    Py::Object Load(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        return LoadImpl(args, kwargs, nullptr);
    }

    Py::Object Loads(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        auto pythonString = ConvertToString(ExtractArgument(args, kwargs, "string"));
        auto string = Stroka(PyString_AsString(*pythonString), pythonString.size());

        std::unique_ptr<TInputStream> stringStream(new TOwningStringInput(string));

        return LoadImpl(args, kwargs, std::move(stringStream));
    }

    Py::Object Dump(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        DumpImpl(args, kwargs, nullptr);

        return Py::None();
    }

    Py::Object Dumps(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        Stroka result;
        TStringOutput stringOutput(result);

        DumpImpl(args, kwargs, &stringOutput);
        return Py::String(~result, result.Size());
    }

    virtual ~TYsonModule()
    { }

private:
    Py::Object LoadImpl(
        Py::Tuple& args,
        Py::Dict& kwargs,
        std::unique_ptr<TInputStream> inputStream)
    {
        // Holds inputStreamWrap if passed non-trivial stream argument
        TInputStream* inputStreamPtr;
        if (!inputStream) {
            auto streamArg = ExtractArgument(args, kwargs, "stream");

            if (PyFile_Check(streamArg.ptr())) {
                FILE* file = PyFile_AsFile(streamArg.ptr());
                inputStream.reset(new TFileInput(Duplicate(file)));
            } else {
                inputStream.reset(new TInputStreamWrap(streamArg));
            }
        }
        inputStreamPtr = inputStream.get();

        auto ysonType = NYson::EYsonType::Node;
        if (HasArgument(args, kwargs, "yson_type")) {
            auto arg = ExtractArgument(args, kwargs, "yson_type");
            ysonType = ParseEnum<NYson::EYsonType>(ConvertToStroka(ConvertToString(arg)));
        }

        bool alwaysCreateAttributes = true;
        if (HasArgument(args, kwargs, "always_create_attributes")) {
            auto arg = ExtractArgument(args, kwargs, "always_create_attributes");
            alwaysCreateAttributes = Py::Boolean(arg);
        }

        bool raw = false;
        if (HasArgument(args, kwargs, "raw")) {
            auto arg = ExtractArgument(args, kwargs, "raw");
            raw = Py::Boolean(arg);
        }

        ValidateArgumentsEmpty(args, kwargs);

        if (ysonType == NYson::EYsonType::MapFragment) {
            throw CreateYsonError("Map fragment is not supported");
        }

        if (ysonType == NYson::EYsonType::ListFragment) {
            if (raw) {
                Py::Callable class_type(TRawYsonIterator::type());
                Py::PythonClassObject<TRawYsonIterator> pythonIter(class_type.apply(Py::Tuple(), Py::Dict()));

                auto* iter = pythonIter.getCxxObject();
                iter->Init(inputStreamPtr, std::move(inputStream));
                return pythonIter;
            } else {
                Py::Callable class_type(TYsonIterator::type());
                Py::PythonClassObject<TYsonIterator> pythonIter(class_type.apply(Py::Tuple(), Py::Dict()));

                auto* iter = pythonIter.getCxxObject();
                iter->Init(inputStreamPtr, std::move(inputStream), alwaysCreateAttributes);
                return pythonIter;
            }
        } else {
            if (raw) {
                THROW_ERROR_EXCEPTION("Raw mode is only supported for list fragments");
            }
            NYTree::TPythonObjectBuilder consumer(alwaysCreateAttributes);
            NYson::TYsonParser parser(&consumer, ysonType);

            const int BufferSize = 1024 * 1024;
            char buffer[BufferSize];
            try {
                while (int length = inputStreamPtr->Read(buffer, BufferSize))
                {
                    parser.Read(TStringBuf(buffer, length));
                    if (BufferSize != length) {
                        break;
                    }
                }
                parser.Finish();
            } catch (const std::exception& error) {
                throw CreateYsonError(error.what());
            }

            return consumer.ExtractObject();
        }
    }

    void DumpImpl(Py::Tuple& args, Py::Dict& kwargs, TOutputStream* outputStream)
    {
        auto obj = ExtractArgument(args, kwargs, "object");

        // Holds outputStreamWrap if passed non-trivial stream argument
        std::unique_ptr<TOutputStreamWrap> outputStreamWrap;
        std::unique_ptr<TFileOutput> fileOutput;
        std::unique_ptr<TBufferedOutput> bufferedOutputStream;

        if (!outputStream) {
            auto streamArg = ExtractArgument(args, kwargs, "stream");

            if (PyFile_Check(streamArg.ptr())) {
                FILE* file = PyFile_AsFile(streamArg.ptr());
                fileOutput.reset(new TFileOutput(Duplicate(file)));
                outputStream = fileOutput.get();
            } else {
                outputStreamWrap.reset(new TOutputStreamWrap(streamArg));
                outputStream = outputStreamWrap.get();
            }
            bufferedOutputStream.reset(new TBufferedOutput(outputStream, 1024 * 1024));
            outputStream = bufferedOutputStream.get();
        }

        auto ysonFormat = NYson::EYsonFormat::Text;
        if (HasArgument(args, kwargs, "yson_format")) {
            auto arg = ExtractArgument(args, kwargs, "yson_format");
            ysonFormat = ParseEnum<NYson::EYsonFormat>(ConvertToStroka(ConvertToString(arg)));
        }

        NYson::EYsonType ysonType = NYson::EYsonType::Node;
        if (HasArgument(args, kwargs, "yson_type")) {
            auto arg = ExtractArgument(args, kwargs, "yson_type");
            ysonType = ParseEnum<NYson::EYsonType>(ConvertToStroka(ConvertToString(arg)));
        }

        int indent = 4;
        if (HasArgument(args, kwargs, "indent")) {
            auto arg = ExtractArgument(args, kwargs, "indent");
            indent = Py::Int(arg).asLongLong();
        }

        bool booleanAsString = true;
        if (HasArgument(args, kwargs, "boolean_as_string")) {
            auto arg = ExtractArgument(args, kwargs, "boolean_as_string");
            booleanAsString = Py::Boolean(arg);
        }

        bool ignoreInnerAttributes = false;
        if (HasArgument(args, kwargs, "ignore_inner_attributes")) {
            auto arg = ExtractArgument(args, kwargs, "ignore_inner_attributes");
            ignoreInnerAttributes = Py::Boolean(arg);
        }

        ValidateArgumentsEmpty(args, kwargs);

        NYson::TYsonWriter writer(outputStream, ysonFormat, ysonType, false, booleanAsString, indent);
        if (ysonType == NYson::EYsonType::Node) {
            try {
                Serialize(obj, &writer, ignoreInnerAttributes);
            } catch (const NYT::TErrorException& error) {
                throw CreateYsonError(error.what());
            }
        } else if (ysonType == NYson::EYsonType::ListFragment) {
            auto iterator = Py::Object(PyObject_GetIter(obj.ptr()), true);
            try {
                PyObject *item;
                while (item = PyIter_Next(*iterator)) {
                    Serialize(Py::Object(item, true), &writer, ignoreInnerAttributes);
                }
                if (PyErr_Occurred()) {
                    throw Py::Exception();
                }
            } catch (const NYT::TErrorException& error) {
                throw CreateYsonError(error.what());
            }
        } else {
            throw CreateYsonError(ToString(ysonType) + " is not supported");
        }

        if (bufferedOutputStream) {
            bufferedOutputStream->Flush();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT

///////////////////////////////////////////////////////////////////////////////

#if defined( _WIN32 )
#define EXPORT_SYMBOL __declspec( dllexport )
#else
#define EXPORT_SYMBOL
#endif

extern "C" EXPORT_SYMBOL void inityson_lib()
{
    static NYT::NPython::TYsonModule* yson = new NYT::NPython::TYsonModule;
    UNUSED(yson);
}

extern "C" EXPORT_SYMBOL void inityson_lib_d()
{
    inityson_lib();
}

