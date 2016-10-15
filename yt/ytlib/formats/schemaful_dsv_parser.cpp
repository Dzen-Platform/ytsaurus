#include "schemaful_dsv_parser.h"
#include "parser.h"
#include "schemaful_dsv_table.h"

#include <yt/ytlib/table_client/public.h>

namespace NYT {
namespace NFormats {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

class TSchemafulDsvParser
    : public IParser
{
public:
    TSchemafulDsvParser(
        IYsonConsumer* consumer,
        TSchemafulDsvFormatConfigPtr config);

    virtual void Read(const TStringBuf& data) override;
    virtual void Finish() override;

private:
    IYsonConsumer* Consumer_;
    TSchemafulDsvFormatConfigPtr Config_;
    const std::vector<Stroka>& Columns_;

    TSchemafulDsvTable Table_;

    bool NewRecordStarted_ = false;
    bool ExpectingEscapedChar_ = false;

    int RowIndex_ = 0;
    int FieldIndex_ = 0;

    int TableIndex_ = 0;

    Stroka CurrentToken_;

    const char* Consume(const char* begin, const char* end);
    void SwitchTable(int newTableIndex);
};

////////////////////////////////////////////////////////////////////////////////

TSchemafulDsvParser::TSchemafulDsvParser(
        IYsonConsumer* consumer,
        TSchemafulDsvFormatConfigPtr config)
    : Consumer_(consumer)
    , Config_(config)
    , Columns_(Config_->GetColumnsOrThrow())
    , Table_(Config_)
{ }

void TSchemafulDsvParser::Read(const TStringBuf& data)
{
    auto current = data.begin();
    while (current != data.end()) {
        current = Consume(current, data.end());
    }
}

const char* TSchemafulDsvParser::Consume(const char* begin, const char* end)
{
    // Process escaping symbols.
    if (Config_->EnableEscaping && !ExpectingEscapedChar_ && *begin == Config_->EscapingSymbol) {
        ExpectingEscapedChar_ = true;
        return begin + 1;
    }
    if (ExpectingEscapedChar_) {
        CurrentToken_.append(Table_.Escapes.Backward[static_cast<ui8>(*begin)]);
        ExpectingEscapedChar_ = false;
        return begin + 1;
    }

    // Process common case.
    auto next = Table_.Stops.FindNext(begin, end);
    CurrentToken_.append(begin, next);
    if (next == end || *next == Config_->EscapingSymbol) {
        return next;
    }

    YCHECK(*next == Config_->FieldSeparator ||
           *next == Config_->RecordSeparator);

    if (!NewRecordStarted_) {
        NewRecordStarted_ = true;

        if (Config_->EnableTableIndex) {
            SwitchTable(FromString<i32>(CurrentToken_));
        }

        Consumer_->OnListItem();
        Consumer_->OnBeginMap();

        if (Config_->EnableTableIndex) {
            CurrentToken_.clear();
            return next + 1;
        }
    }

    if (FieldIndex_ == Columns_.size()) {
        THROW_ERROR_EXCEPTION("Too many fields in row (expected %v)", Columns_.size());
    }

    Consumer_->OnKeyedItem(Columns_[FieldIndex_++]);
    Consumer_->OnStringScalar(CurrentToken_);
    CurrentToken_.clear();

    if (*next == Config_->RecordSeparator) {
        if (FieldIndex_ != Columns_.size()) {
            THROW_ERROR_EXCEPTION("Row %v is incomplete: expected %v fields but found %v",
                RowIndex_,
                Columns_.size(),
                FieldIndex_);
        }
        Consumer_->OnEndMap();
        NewRecordStarted_ = false;
        FieldIndex_ = 0;

        RowIndex_ += 1;
    }
    return next + 1;
}

void TSchemafulDsvParser::SwitchTable(int newTableIndex)
{
    static const Stroka key = FormatEnum(NTableClient::EControlAttribute(
        NTableClient::EControlAttribute::TableIndex));
    if (newTableIndex != TableIndex_) {
        TableIndex_ = newTableIndex;

        Consumer_->OnListItem();
        Consumer_->OnBeginAttributes();
        Consumer_->OnKeyedItem(key);
        Consumer_->OnInt64Scalar(TableIndex_);
        Consumer_->OnEndAttributes();
        Consumer_->OnEntity();
    }
}

void TSchemafulDsvParser::Finish()
{
    if (NewRecordStarted_ || !CurrentToken_.Empty() || ExpectingEscapedChar_) {
        THROW_ERROR_EXCEPTION("Row %v is not finished", RowIndex_);
    }
    CurrentToken_.clear();
}

////////////////////////////////////////////////////////////////////////////////

void ParseSchemafulDsv(
    TInputStream* input,
    IYsonConsumer* consumer,
    TSchemafulDsvFormatConfigPtr config)
{
    auto parser = CreateParserForSchemafulDsv(consumer, config);
    Parse(input, parser.get());
}

void ParseSchemafulDsv(
    const TStringBuf& data,
    IYsonConsumer* consumer,
    TSchemafulDsvFormatConfigPtr config)
{
    auto parser = CreateParserForSchemafulDsv(consumer, config);
    parser->Read(data);
    parser->Finish();
}

std::unique_ptr<IParser> CreateParserForSchemafulDsv(
    IYsonConsumer* consumer,
    TSchemafulDsvFormatConfigPtr config)
{
    if (config->EnableColumnNamesHeader) {
        THROW_ERROR_EXCEPTION("Parameter %Qv must not be specified for schemaful dsv parser", "enable_column_names_header");
    }
    return std::unique_ptr<IParser>(new TSchemafulDsvParser(consumer, config));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
