#pragma once

#include "lazy_dict_producer.h"

#include <yt/yt/python/common/helpers.h>
#include <yt/yt/python/common/stream.h>

#include <yt/yt/core/yson/consumer.h>
#include <yt/yt/core/yson/lexer_detail.h>
#include <yt/yt/core/yson/parser.h>

#include <Python.h>

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

class TLazyYsonConsumer
    : public NYson::IYsonConsumer
{
public:
    TLazyYsonConsumer(
        TCallback<TSharedRef()> extractPrefixCallback_,
        TPythonStringCache* keyCacher,
        const std::optional<TString>& encoding,
        bool alwaysCreateAttributes);

    void OnListItem();
    void OnKeyedItem(TStringBuf key);
    void OnBeginAttributes();
    void OnEndAttributes();
    void OnRaw(TStringBuf /*yson*/, NYson::EYsonType /*type*/);
    void OnStringScalar(TStringBuf value);
    void OnInt64Scalar(i64 value);
    void OnUint64Scalar(ui64 value);
    void OnDoubleScalar(double value);
    void OnBooleanScalar(bool value);
    void OnEntity();
    void OnBeginList();
    void OnEndList();
    void OnBeginMap();
    void OnEndMap();

    bool HasObject() const;

    Py::Object ExtractObject();

private:
    void OnItemConsumed();
    void OnItem();

    int Balance_ = 0;

    std::queue<Py::Object> Objects_;

    TCallback<TSharedRef()> ExtractPrefixCallback_;

    TPythonStringCache* KeyCacher_;
    std::optional<PyObjectPtr> ItemKey_;

    std::unique_ptr<TLazyDictProducer> LazyDictConsumer_;

    bool IsLazyDictObject_ = true;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
