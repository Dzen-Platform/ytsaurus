#pragma once

#include "public.h"

#include <yt/client/complex_types/named_structures_yson.h>
#include <yt/client/formats/public.h>

#include <yt/client/table_client/value_consumer.h>
#include <yt/client/table_client/name_table.h>

#include <yt/core/misc/error.h>

#include <yt/core/yson/consumer.h>
#include <yt/core/yson/writer.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TYsonToUnversionedValueConverter
    : public NYson::TYsonConsumerBase
{
public:
    DEFINE_BYREF_RO_PROPERTY(std::vector<IValueConsumer*>, ValueConsumers);

public:
    TYsonToUnversionedValueConverter(
        NFormats::EComplexTypeMode complexTypeMode,
        IValueConsumer* valueConsumers);

    TYsonToUnversionedValueConverter(
        NFormats::EComplexTypeMode complexTypeMode,
        std::vector<IValueConsumer*> valueConsumers,
        int tableIndex = 0);

    IValueConsumer* SwitchToTable(int tableIndex);

    // Set column index of next emitted value.
    void SetColumnIndex(int columnIndex);

    int GetDepth() const;

    virtual void OnStringScalar(TStringBuf value) override;
    virtual void OnInt64Scalar(i64 value) override;
    virtual void OnUint64Scalar(ui64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnBooleanScalar(bool value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(TStringBuf name) override;
    virtual void OnEndMap() override;
    virtual void OnBeginAttributes() override;
    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;

private:
    TBlobOutput ValueBuffer_;
    NYson::TBufferedBinaryYsonWriter ValueWriter_;

    THashMap<std::pair<int,int>, NComplexTypes::TYsonConverter> Converters_;
    TBlobOutput ConvertedBuffer_;
    NYson::TBufferedBinaryYsonWriter ConvertedWriter_;

    IValueConsumer* CurrentValueConsumer_;
    int Depth_ = 0;
    int ColumnIndex_ = 0;
    int TableIndex_ = 0;

private:
    void FlushCurrentValueIfCompleted();
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETableConsumerControlState,
    (None)
    (ExpectName)
    (ExpectValue)
    (ExpectEndAttributes)
    (ExpectEntity)
);

class TTableConsumer
    : public NYson::TYsonConsumerBase
{
public:
    TTableConsumer(
        NFormats::EComplexTypeMode complexTypeMode,
        IValueConsumer* consumer);
    TTableConsumer(
        NFormats::EComplexTypeMode complexTypeMode,
        std::vector<IValueConsumer*> consumers,
        int tableIndex = 0);

protected:
    using EControlState = ETableConsumerControlState;

    TError AttachLocationAttributes(TError error) const;

    virtual void OnStringScalar(TStringBuf value) override;
    virtual void OnInt64Scalar(i64 value) override;
    virtual void OnUint64Scalar(ui64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnBooleanScalar(bool value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(TStringBuf name) override;
    virtual void OnEndMap() override;

    virtual void OnBeginAttributes() override;

    void ThrowMapExpected() const;
    void ThrowEntityExpected() const;
    void ThrowControlAttributesNotSupported() const;
    void ThrowInvalidControlAttribute(const TString& whatsWrong) const;

    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;

    void OnControlInt64Scalar(i64 value);
    void OnControlStringScalar(TStringBuf value);

    void SwitchToTable(int tableIndex);

private:
    int GetTableCount() const;

protected:
    std::vector<std::unique_ptr<TNameTableWriter>> NameTableWriters_;

    IValueConsumer* CurrentValueConsumer_ = nullptr;
    TNameTableWriter* CurrentNameTableWriter_ = nullptr;

    EControlState ControlState_ = EControlState::None;
    EControlAttribute ControlAttribute_;

    TYsonToUnversionedValueConverter YsonToUnversionedValueConverter_;

    int Depth_ = 0;

    i64 RowIndex_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
