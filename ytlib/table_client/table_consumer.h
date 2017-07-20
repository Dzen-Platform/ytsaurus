#pragma once

#include "public.h"
#include "value_consumer.h"
#include "name_table.h"

#include <yt/core/misc/error.h>

#include <yt/core/yson/consumer.h>
#include <yt/core/yson/writer.h>

namespace NYT {
namespace NTableClient {

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
    explicit TTableConsumer(
        IValueConsumer* consumer);
    explicit TTableConsumer(
        std::vector<IValueConsumer*> consumers,
        int tableIndex = 0);

protected:
    using EControlState = ETableConsumerControlState;

    TError AttachLocationAttributes(TError error) const;

    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnInt64Scalar(i64 value) override;
    virtual void OnUint64Scalar(ui64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnBooleanScalar(bool value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(const TStringBuf& name) override;
    virtual void OnEndMap() override;

    virtual void OnBeginAttributes() override;

    void ThrowMapExpected() const;
    void ThrowEntityExpected() const;
    void ThrowControlAttributesNotSupported() const;
    void ThrowInvalidControlAttribute(const TString& whatsWrong) const;

    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;

    void OnControlInt64Scalar(i64 value);
    void OnControlStringScalar(const TStringBuf& value);

    void FlushCurrentValueIfCompleted();

    void SwitchToTable(int tableIndex);


    const std::vector<IValueConsumer*> ValueConsumers_;
    std::vector<std::unique_ptr<TNameTableWriter>> NameTableWriters_;

    IValueConsumer* CurrentValueConsumer_ = nullptr;
    TNameTableWriter* CurrentNameTableWriter_ = nullptr;

    EControlState ControlState_ = EControlState::None;
    EControlAttribute ControlAttribute_;

    TBlobOutput ValueBuffer_;
    NYson::TBufferedBinaryYsonWriter ValueWriter_;

    int Depth_ = 0;
    int ColumnIndex_ = 0;

    i64 RowIndex_ = 0;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
