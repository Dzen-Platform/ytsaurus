#pragma once
#ifndef PARSER_INL_H_
#error "Direct inclusion of this file is not allowed, include parser.h"
// For the sake of sane code completion.
#include "parser.h"
#endif

#include "skiff.h"

#include <yt/core/concurrency/coroutine.h>

namespace NYT::NSkiff {

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
                Consumer_->OnYsonString(Parser_->ParseYson32(), columnId);
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
                Consumer_->OnStringScalar(Parser_->ParseString32(), columnId);
                break;
            default:
                // Other types should be filtered out when we parse skiff schema.
                Y_UNREACHABLE();
        }
    }

    void DoParse(IZeroCopyInput* stream)
    {
        Parser_ = std::make_unique<TCheckedInDebugSkiffParser>(CreateVariant16Schema(SkiffSchemaList_), stream);

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
                auto buf = Parser_->ParseYson32();
                Consumer_->OnOtherColumns(buf);
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

    const std::vector<TSkiffTableColumnIds> TablesColumnIds_;
    std::vector<TSkiffTableDescription> TableDescriptions_;
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
    , ParserCoroPipe_(BIND(
        [=] (IZeroCopyInput* stream) {
            ParserImpl_->DoParse(stream);
        }))
{ }

template <class TConsumer>
TSkiffMultiTableParser<TConsumer>::~TSkiffMultiTableParser()
{ }

template <class TConsumer>
void TSkiffMultiTableParser<TConsumer>::Read(TStringBuf data)
{
    ParserCoroPipe_.Feed(data);
}

template <class TConsumer>
void TSkiffMultiTableParser<TConsumer>::Finish()
{
    ParserCoroPipe_.Finish();
}

template <class TConsumer>
ui64 TSkiffMultiTableParser<TConsumer>::GetReadBytesCount()
{
    return ParserImpl_->GetReadBytesCount();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSkiff
