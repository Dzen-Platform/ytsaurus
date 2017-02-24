#include "public.h"
#include "helpers.h"
#include "serialize.h"
#include "shutdown.h"
#include "stream.h"

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/misc/crash_handler.h>

#include <contrib/libs/pycxx/Extensions.hxx>
#include <contrib/libs/pycxx/Objects.hxx>

namespace NYT {
namespace NPython {

using namespace NYTree;
using namespace NYPath;

///////////////////////////////////////////////////////////////////////////////

Py::Exception CreateYsonError(const Stroka& message, const NYT::TError& error = TError())
{
    auto ysonModule = Py::Module(PyImport_ImportModule("yt.yson.common"), true);
    auto ysonErrorClass = Py::Callable(GetAttr(ysonModule, "YsonError"));

    std::vector<TError> innerErrors({error});
    Py::Dict options;
    options.setItem("message", ConvertTo<Py::Object>(message));
    options.setItem("code", ConvertTo<Py::Object>(1));
    options.setItem("inner_errors", ConvertTo<Py::Object>(innerErrors));
    auto ysonError = ysonErrorClass.apply(Py::Tuple(), options);
    return Py::Exception(*ysonError.type(), ysonError);
}

#define CATCH(message) \
    catch (const NYT::TErrorException& error) { \
        throw CreateYsonError(message, error.Error()); \
    } catch (const std::exception& ex) { \
        if (PyErr_ExceptionMatches(PyExc_BaseException)) { \
            throw; \
        } else { \
            throw CreateYsonError(message, TError(ex)); \
        } \
    }

///////////////////////////////////////////////////////////////////////////////

class TYsonIterator
    : public Py::PythonClass<TYsonIterator>
{
public:
    TYsonIterator(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
        : Py::PythonClass<TYsonIterator>::PythonClass(self, args, kwargs)
    { }

    void Init(TInputStream* inputStream, std::unique_ptr<TInputStream> inputStreamOwner,
              bool alwaysCreateAttributes, const TNullable<Stroka>& encoding)
    {
        YCHECK(!inputStreamOwner || inputStreamOwner.get() == inputStream);
        InputStream_ = inputStream;
        InputStreamOwner_ = std::move(inputStreamOwner);
        Consumer_.reset(new NYTree::TPythonObjectBuilder(alwaysCreateAttributes, encoding));
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
        } CATCH("Yson load failed");
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
        Lexer_ = TListFragmentLexer(inputStream);
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
            auto result = Py::Bytes(item.Begin(), item.Size());
            result.increment_reference_count();
            return result.ptr();
        } CATCH("Yson load failed");
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
        InstallCrashSignalHandler(std::set<int>({SIGSEGV}));

        TYsonIterator::InitType();
        TRawYsonIterator::InitType();

        add_keyword_method("load", &TYsonModule::Load, "Loads YSON from stream");
        add_keyword_method("loads", &TYsonModule::Loads, "Loads YSON from string");

        add_keyword_method("dump", &TYsonModule::Dump, "Dumps YSON to stream");
        add_keyword_method("dumps", &TYsonModule::Dumps, "Dumps YSON to string");

        add_keyword_method("parse_ypath", &TYsonModule::ParseYPath, "Parse YPath");

        initialize("Python bindings for YSON");
    }

    Py::Object Load(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        try {
            return LoadImpl(args, kwargs, nullptr);
        } CATCH("Yson load failed");
    }

    Py::Object Loads(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        auto stringArgument = ExtractArgument(args, kwargs, "string");
#if PY_MAJOR_VERSION >= 3
        if (PyUnicode_Check(stringArgument.ptr())) {
            throw Py::TypeError("Only binary strings parsing is supported, got unicode");
        }
#endif
        auto string = ConvertStringObjectToStroka(stringArgument);
        std::unique_ptr<TInputStream> stringStream(new TOwningStringInput(string));

        try {
            return LoadImpl(args, kwargs, std::move(stringStream));
        } CATCH("Yson load failed");
    }

    Py::Object Dump(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        try {
            DumpImpl(args, kwargs, nullptr);
        } CATCH("Yson dumps failed");

        return Py::None();
    }

    Py::Object Dumps(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        Stroka result;
        TStringOutput stringOutput(result);

        try {
            DumpImpl(args, kwargs, &stringOutput);
        } CATCH("Yson dumps failed");

        return Py::ConvertToPythonString(result);
    }

    Py::Object ParseYPath(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        auto path = ConvertStringObjectToStroka(ExtractArgument(args, kwargs, "path"));
        ValidateArgumentsEmpty(args, kwargs);

        auto richPath = TRichYPath::Parse(path);
        return CreateYsonObject("YsonString", Py::Bytes(richPath.GetPath()), ConvertTo<Py::Object>(richPath.Attributes().ToMap()));
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
            bool wrapStream = true;
#if PY_MAJOR_VERSION < 3
            if (PyFile_Check(streamArg.ptr())) {
                FILE* file = PyFile_AsFile(streamArg.ptr());
                inputStream.reset(new TFileInput(Duplicate(file)));
                wrapStream = false;
            }
#endif
            if (wrapStream) {
                inputStream.reset(new TInputStreamWrap(streamArg));
            }
        }
        inputStreamPtr = inputStream.get();

        auto ysonType = NYson::EYsonType::Node;
        if (HasArgument(args, kwargs, "yson_type")) {
            auto arg = ExtractArgument(args, kwargs, "yson_type");
                ysonType = ParseEnum<NYson::EYsonType>(ConvertStringObjectToStroka(arg));
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

        TNullable<Stroka> encoding;
        if (HasArgument(args, kwargs, "encoding")) {
            auto arg = ExtractArgument(args, kwargs, "encoding");
            if (!arg.isNone()) {
#if PY_MAJOR_VERSION < 3
                throw CreateYsonError("Encoding parameter is not supported for Python 2");
#else
                encoding = ConvertStringObjectToStroka(arg);
#endif
            }
#if PY_MAJOR_VERSION >= 3
        } else {
            encoding = "utf-8";
#endif
        }

        ValidateArgumentsEmpty(args, kwargs);

        if (ysonType == NYson::EYsonType::ListFragment) {
            if (raw) {
                Py::Callable classType(TRawYsonIterator::type());
                Py::PythonClassObject<TRawYsonIterator> pythonIter(classType.apply(Py::Tuple(), Py::Dict()));

                auto* iter = pythonIter.getCxxObject();
                iter->Init(inputStreamPtr, std::move(inputStream));
                return pythonIter;
            } else {
                Py::Callable classType(TYsonIterator::type());
                Py::PythonClassObject<TYsonIterator> pythonIter(classType.apply(Py::Tuple(), Py::Dict()));

                auto* iter = pythonIter.getCxxObject();
                iter->Init(inputStreamPtr, std::move(inputStream), alwaysCreateAttributes, encoding);
                return pythonIter;
            }
        } else {
            if (raw) {
                throw CreateYsonError("Raw mode is only supported for list fragments");
            }
            NYTree::TPythonObjectBuilder consumer(alwaysCreateAttributes, encoding);
            NYson::TYsonParser parser(&consumer, ysonType);

            const int BufferSize = 1024 * 1024;
            char buffer[BufferSize];

            if (ysonType == NYson::EYsonType::MapFragment) {
                consumer.OnBeginMap();
            }
            while (int length = inputStreamPtr->Read(buffer, BufferSize))
            {
                parser.Read(TStringBuf(buffer, length));
                if (BufferSize != length) {
                    break;
                }
            }
            parser.Finish();
            if (ysonType == NYson::EYsonType::MapFragment) {
                consumer.OnEndMap();
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
            bool wrapStream = true;
#if PY_MAJOR_VERSION < 3
            if (PyFile_Check(streamArg.ptr())) {
                FILE* file = PyFile_AsFile(streamArg.ptr());
                fileOutput.reset(new TFileOutput(Duplicate(file)));
                outputStream = fileOutput.get();
                wrapStream = false;
            }
#endif
            if (wrapStream) {
                outputStreamWrap.reset(new TOutputStreamWrap(streamArg));
                outputStream = outputStreamWrap.get();
            }
#if PY_MAJOR_VERSION < 3
            // Python 3 has "io" module with fine-grained buffering control, no need in
            // additional buferring here.
            bufferedOutputStream.reset(new TBufferedOutput(outputStream, 1024 * 1024));
            outputStream = bufferedOutputStream.get();
#endif
        }

        auto ysonFormat = NYson::EYsonFormat::Text;
        if (HasArgument(args, kwargs, "yson_format")) {
            auto arg = ExtractArgument(args, kwargs, "yson_format");
            ysonFormat = ParseEnum<NYson::EYsonFormat>(ConvertStringObjectToStroka(arg));
        }

        NYson::EYsonType ysonType = NYson::EYsonType::Node;
        if (HasArgument(args, kwargs, "yson_type")) {
            auto arg = ExtractArgument(args, kwargs, "yson_type");
            ysonType = ParseEnum<NYson::EYsonType>(ConvertStringObjectToStroka(arg));
        }

        int indent = NYson::TYsonWriter::DefaultIndent;
        const int maxIndentValue = 128;

        if (HasArgument(args, kwargs, "indent")) {
            auto arg = Py::Int(ExtractArgument(args, kwargs, "indent"));
            if (arg > maxIndentValue) {
                throw CreateYsonError(Format("Indent value exceeds indentation limit (%d)", maxIndentValue));
            }
            indent = static_cast<int>(Py::Long(arg).as_long());
        }

        bool booleanAsString = false;
        if (HasArgument(args, kwargs, "boolean_as_string")) {
            auto arg = ExtractArgument(args, kwargs, "boolean_as_string");
            booleanAsString = Py::Boolean(arg);
        }

        bool ignoreInnerAttributes = false;
        if (HasArgument(args, kwargs, "ignore_inner_attributes")) {
            auto arg = ExtractArgument(args, kwargs, "ignore_inner_attributes");
            ignoreInnerAttributes = Py::Boolean(arg);
        }

        TNullable<Stroka> encoding("utf-8");
        if (HasArgument(args, kwargs, "encoding")) {
            auto arg = ExtractArgument(args, kwargs, "encoding");
            if (arg.isNone()) {
                encoding = Null;
            } else {
                encoding = ConvertStringObjectToStroka(arg);
            }
        }

        ValidateArgumentsEmpty(args, kwargs);

        auto writer = NYson::CreateYsonWriter(
            outputStream,
            ysonFormat,
            ysonType,
            false,
            booleanAsString,
            indent);

        switch (ysonType) {
            case NYson::EYsonType::Node:
            case NYson::EYsonType::MapFragment:
                Serialize(obj, writer.get(), encoding, ignoreInnerAttributes, ysonType);
                break;

            case NYson::EYsonType::ListFragment: {
                auto iterator = Py::Object(PyObject_GetIter(obj.ptr()), true);
                while (auto* item = PyIter_Next(*iterator)) {
                    Serialize(Py::Object(item, true), writer.get(), encoding, ignoreInnerAttributes);
                }
                if (PyErr_Occurred()) {
                    throw Py::Exception();
                }
                break;
            }

            default:
                throw CreateYsonError("YSON type " + ToString(ysonType) + " is not supported");
        }

        writer->Flush();

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

static PyObject* init_module()
{
    static NYT::NPython::TYsonModule* yson = new NYT::NPython::TYsonModule;
    return yson->module().ptr();
}

#if PY_MAJOR_VERSION < 3
extern "C" EXPORT_SYMBOL void inityson_lib() { Y_UNUSED(init_module()); }
extern "C" EXPORT_SYMBOL void inityson_lib_d() { inityson_lib(); }
#else
extern "C" EXPORT_SYMBOL PyObject* PyInit_yson_lib() { return init_module(); }
extern "C" EXPORT_SYMBOL PyObject* PyInit_yson_lib_d() { return PyInit_yson_lib(); }
#endif
