#pragma once
#ifndef PARSER_INL_H_
#error "Direct inclusion of this file is not allowed, include parser.h"
// For the sake of sane code completion.
#include "parser.h"
#endif

#include "skiff.h"

#include <yt/core/concurrency/coroutine.h>

namespace NYT::NSkiff {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TCoroStream
    : public IZeroCopyInput
{
public:
    TCoroStream(TStringBuf data, TCoroutine<void(TStringBuf)>* coroutine)
        : Coroutine_(coroutine)
        , PendingData_(data)
        , Finished_(data.empty())
    { }

    size_t DoNext(const void** ptr, size_t len) override
    {
        if (PendingData_.empty()) {
            if (Finished_) {
                *ptr = nullptr;
                return 0;
            }
            std::tie(PendingData_) = Coroutine_->Yield();
            if (PendingData_.empty()) {
                Finished_ = true;
                *ptr = nullptr;
                return 0;
            }
        }
        *ptr = PendingData_.data();
        len = Min(len, PendingData_.size());
        PendingData_.Skip(len);
        return len;
    }

    void Complete()
    {
        if (!Finished_) {
            const void* ptr;
            if (!PendingData_.empty() || DoNext(&ptr, 1)) {
                THROW_ERROR_EXCEPTION("Stray data in stream");
            }
        }
    }

private:
    TCoroutine<void(TStringBuf)>* const Coroutine_;
    TStringBuf PendingData_;
    bool Finished_ = false;
};

////////////////////////////////////////////////////////////////////////////////

template <class TConsumer>
class TSkiffMultiTableParser<TConsumer>::TImpl
{
public:
    TImpl(
        TConsumer* consumer,
        const TSkiffSchemaList& skiffSchemaList,
        const std::vector<TSkiffTableColumnIds>& tablesColumnIds,
        const TString& rangeIndexColumnName,
        const TString& rowIndexColumnName)
        : Consumer_(consumer)
        , SkiffSchemaList_(skiffSchemaList)
        , TablesColumnIds_(tablesColumnIds)
    {
        TableDescriptions_ = CreateTableDescriptionList(SkiffSchemaList_, rangeIndexColumnName, rowIndexColumnName);

        YCHECK(tablesColumnIds.size() == TableDescriptions_.size());
        for (size_t index = 0; index < TableDescriptions_.size(); ++index) {
            YCHECK(tablesColumnIds[index].DenseFieldColumnIds.size() == TableDescriptions_[index].DenseFieldDescriptionList.size());
            YCHECK(tablesColumnIds[index].SparseFieldColumnIds.size() == TableDescriptions_[index].SparseFieldDescriptionList.size());
        }
    }

    Y_FORCE_INLINE void ParseField(ui16 columnId, const TString& name, EWireType wireType, bool required = false)
    {
        if (!required) {
            ui8 tag = Parser_->ParseVariant8Tag();
            if (tag == 0) {
                Consumer_->OnEntity(columnId);
                return;
            } else if (tag > 1) {
                THROW_ERROR_EXCEPTION(
                    "Found bad variant8 tag %Qv when parsing optional field %Qv",
                    tag,
                    name);
            }
        }
        switch (wireType) {
            case EWireType::Yson32:
                Parser_->ParseYson32(&String_);
                Consumer_->OnYsonString(String_, columnId);
                break;
            case EWireType::Int64:
                Consumer_->OnInt64Scalar(Parser_->ParseInt64(), columnId);
                break;
            case EWireType::Uint64:
                Consumer_->OnUint64Scalar(Parser_->ParseUint64(), columnId);
                break;
            case EWireType::Double:
                Consumer_->OnDoubleScalar(Parser_->ParseDouble(), columnId);
                break;
            case EWireType::Boolean:
                Consumer_->OnBooleanScalar(Parser_->ParseBoolean(), columnId);
                break;
            case EWireType::String32:
                Parser_->ParseString32(&String_);
                Consumer_->OnStringScalar(String_, columnId);
                break;
            default:
                // Other types should be filtered out when we parse skiff schema.
                Y_UNREACHABLE();
        }
    }

    void DoParse(TCoroStream* stream)
    {
        Parser_ = std::make_unique<TCheckedInDebugSkiffParser>(CreateVariant16Schema(SkiffSchemaList_), stream);
        InputStream_ = stream;

        while (Parser_->HasMoreData()) {
            auto tag = Parser_->ParseVariant16Tag();
            if (tag >= TableDescriptions_.size()) {
                THROW_ERROR_EXCEPTION("Unknown table index varint16 tag")
                    << TErrorAttribute("tag", tag);
            }

            Consumer_->OnBeginRow(tag);

            for (ui16 i = 0; i < TableDescriptions_[tag].DenseFieldDescriptionList.size(); ++i) {
                const auto& field = TableDescriptions_[tag].DenseFieldDescriptionList[i];
                auto columnId = TablesColumnIds_[tag].DenseFieldColumnIds[i];
                ParseField(columnId, field.Name, field.DeoptionalizedSchema->GetWireType(), field.Required);
            }

            if (!TableDescriptions_[tag].SparseFieldDescriptionList.empty()) {
                for (auto sparseFieldIdx = Parser_->ParseVariant16Tag();
                    sparseFieldIdx != EndOfSequenceTag<ui16>();
                    sparseFieldIdx = Parser_->ParseVariant16Tag())
                {
                    if (sparseFieldIdx >= TableDescriptions_[tag].SparseFieldDescriptionList.size()) {
                        THROW_ERROR_EXCEPTION("Bad sparse field index %Qv, total sparse field count %Qv",
                            sparseFieldIdx,
                            TableDescriptions_[tag].SparseFieldDescriptionList.size());
                    }

                    const auto& field = TableDescriptions_[tag].SparseFieldDescriptionList[sparseFieldIdx];
                    auto columnId = TablesColumnIds_[tag].SparseFieldColumnIds[sparseFieldIdx];
                    ParseField(columnId, field.Name, field.DeoptionalizedSchema->GetWireType(), true);
                }
            }

            if (TableDescriptions_[tag].HasOtherColumns) {
                Parser_->ParseYson32(&String_);
                Consumer_->OnOtherColumns(String_);
            }

            Consumer_->OnEndRow();
        }
    }

    ui64 GetReadBytesCount()
    {
        return Parser_->GetReadBytesCount();
    }

private:
    TConsumer* const Consumer_;
    TSkiffSchemaList SkiffSchemaList_;

    std::unique_ptr<TCheckedInDebugSkiffParser> Parser_;
    TCoroStream* InputStream_;

    const std::vector<TSkiffTableColumnIds> TablesColumnIds_;
    std::vector<TSkiffTableDescription> TableDescriptions_;

    // String that we parse string32 into.
    TString String_;
};

////////////////////////////////////////////////////////////////////////////////

template <class TConsumer>
TSkiffMultiTableParser<TConsumer>::TSkiffMultiTableParser(
    TConsumer* consumer,
    TSkiffSchemaList schemaList,
    const std::vector<TSkiffTableColumnIds>& tablesColumnIds,
    const TString& rangeIndexColumnName,
    const TString& rowIndexColumnName)
    : ParserImpl_(new TImpl(consumer,
        schemaList,
        tablesColumnIds,
        rangeIndexColumnName,
        rowIndexColumnName))
    , ParserCoroutine_(BIND(
        [=] (TParserCoroutine& self, TStringBuf data) {
            TCoroStream stream(data, &self);
            ParserImpl_->DoParse(&stream);
            stream.Complete();
        }))
{ }

template <class TConsumer>
TSkiffMultiTableParser<TConsumer>::~TSkiffMultiTableParser()
{ }

template <class TConsumer>
void TSkiffMultiTableParser<TConsumer>::Read(TStringBuf data)
{
    if (!ParserCoroutine_.IsCompleted()) {
        ParserCoroutine_.Run(data);
    } else {
        THROW_ERROR_EXCEPTION("Input is already parsed");
    }
}

template <class TConsumer>
void TSkiffMultiTableParser<TConsumer>::Finish()
{
    Read(TStringBuf());
}

template <class TConsumer>
ui64 TSkiffMultiTableParser<TConsumer>::GetReadBytesCount()
{
    return ParserImpl_->GetReadBytesCount();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSkiff
