#include "lazy_dict_producer.h"

namespace NYT::NPython {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TLazyDictProducer::TLazyDictProducer() = default;

TLazyDictProducer::TLazyDictProducer(const std::optional<TString>& encoding, bool alwaysCreateAttributes)
    : PythonObjectBuilder_(alwaysCreateAttributes, encoding)
{
    Py::Object encodingParam;
    if (encoding) {
        encodingParam = Py::String(*encoding);
    } else {
        encodingParam = Py::None();
    }
    ParserParams_ = Py::TupleN(encodingParam, Py::Boolean(alwaysCreateAttributes));

    Reset();
}

void TLazyDictProducer::Reset()
{
    ResultObject_ = Py::Object(LazyYsonMapNew(TLazyYsonMapType, Py_None, Py_None), /* owned */ true);
    LazyYsonMapInit(reinterpret_cast<TLazyYsonMap*>(ResultObject_.ptr()), ParserParams_.ptr(), Py::Dict().ptr());

    TLazyYsonMap* object = reinterpret_cast<TLazyYsonMap*>(ResultObject_.ptr());
    TLazyYsonMapBase* attributes = reinterpret_cast<TLazyYsonMapBase*>(object->Attributes);
    LazyDict_ = object->super.Dict;
    LazyAttributesDict_ = attributes->Dict;
}

Py::Object TLazyDictProducer::ExtractObject()
{
    auto result = ResultObject_;
    Reset();
    return result;
}

void TLazyDictProducer::OnBeginAttributes()
{
    InsideAttributes_ = true;
}

void TLazyDictProducer::OnEndAttributes()
{
    InsideAttributes_ = false;
}

void TLazyDictProducer::SetObject()
{
    auto object = GetPythonObjectBuilder()->ExtractObject();
    if (LazyAttributesDict_->Length() > 0) {
        if (object.isNone()) {
            object = Py::Callable(GetYsonTypeClass("YsonEntity"), /* owned */ true).apply(Py::Tuple());
        }
        auto attributes = PyObject_GetAttrString(ResultObject_.ptr(), "attributes");
        YT_VERIFY(PyObject_SetAttrString(object.ptr(), "attributes", attributes) == 0);
        Py_DECREF(attributes);
    }

    ResultObject_ = object;
}

void TLazyDictProducer::OnKeyValue(const Py::Object& key, const TSharedRef& value)
{
    if (InsideAttributes_) {
        LazyAttributesDict_->SetItem(key, value);
    } else {
        LazyDict_->SetItem(key, value);
    }
}

NYTree::TPythonObjectBuilder* TLazyDictProducer::GetPythonObjectBuilder()
{
    return &PythonObjectBuilder_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
