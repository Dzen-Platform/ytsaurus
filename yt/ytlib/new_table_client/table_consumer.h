#pragma once

#include "public.h"
#include "unversioned_row.h"

#include <core/misc/blob_output.h>
#include <core/misc/error.h>

#include <core/yson/consumer.h>
#include <core/yson/writer.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IValueConsumer
    : public virtual TRefCounted
{
    virtual TNameTablePtr GetNameTable() const = 0;

    virtual bool GetAllowUnknownColumns() const = 0;

    virtual void OnBeginRow() = 0;
    virtual void OnValue(const TUnversionedValue& value) = 0;
    virtual void OnEndRow() = 0;

};

DEFINE_REFCOUNTED_TYPE(IValueConsumer)

////////////////////////////////////////////////////////////////////////////////

class TBuildingValueConsumer
    : public IValueConsumer
{
public:
    TBuildingValueConsumer(
        const TTableSchema& schema,
        const TKeyColumns& keyColumns);

    const std::vector<TUnversionedOwningRow>& GetOwningRows() const;
    std::vector<TUnversionedRow> GetRows() const;

    virtual TNameTablePtr GetNameTable() const override;

    void SetTreatMissingAsNull(bool value);

private:
    TUnversionedOwningRowBuilder Builder_;
    std::vector<TUnversionedOwningRow> Rows_;

    TTableSchema Schema_;
    TKeyColumns KeyColumns_;
    TNameTablePtr NameTable_;

    std::vector<bool> WrittenFlags_;
    bool TreatMissingAsNull_ = false;

    TBlobOutput ValueBuffer_;
    NYson::TYsonWriter ValueWriter_;

    virtual bool GetAllowUnknownColumns() const override;

    virtual void OnBeginRow() override;
    virtual void OnValue(const TUnversionedValue& value) override;
    virtual void OnEndRow() override;

    TUnversionedValue MakeAnyFromScalar(const TUnversionedValue& value);
};

DEFINE_REFCOUNTED_TYPE(TBuildingValueConsumer)

////////////////////////////////////////////////////////////////////////////////

class TWritingValueConsumer
    : public IValueConsumer
{
public:
    explicit TWritingValueConsumer(
        ISchemalessWriterPtr writer,
        bool flushImmediately = false);

    void Flush();

private:
    ISchemalessWriterPtr Writer_;

    TUnversionedOwningRowBuilder Builder_;
    std::vector<TUnversionedOwningRow> OwningRows_;
    std::vector<TUnversionedRow> Rows_;

    i64 CurrentBufferSize_ = 0;
    bool FlushImmediately_;

    virtual TNameTablePtr GetNameTable() const override;

    virtual bool GetAllowUnknownColumns() const override;

    virtual void OnBeginRow() override;
    virtual void OnValue(const TUnversionedValue& value) override;
    virtual void OnEndRow() override;

};

DEFINE_REFCOUNTED_TYPE(TWritingValueConsumer)

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
    explicit TTableConsumer(IValueConsumerPtr consumer);
    explicit TTableConsumer(
        const std::vector<IValueConsumerPtr>& consumers,
        int tableIndex = 0);

protected:
    using EControlState = ETableConsumerControlState;

    TError AttachLocationAttributes(TError error);

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

    void ThrowMapExpected();
    void ThrowCompositesNotSupported();
    void ThrowControlAttributesNotSupported();
    void ThrowInvalidControlAttribute(const Stroka& whatsWrong);

    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;

    void OnControlInt64Scalar(i64 value);
    void OnControlStringScalar(const TStringBuf& value);


    void FlushCurrentValueIfCompleted();

    std::vector<IValueConsumerPtr> ValueConsumers_;
    IValueConsumer* CurrentValueConsumer_;

    EControlState ControlState_ = EControlState::None;
    EControlAttribute ControlAttribute_;

    TBlobOutput ValueBuffer_;
    NYson::TYsonWriter ValueWriter_;

    int Depth_ = 0;
    int ColumnIndex_ = 0;

    i64 RowIndex_ = 0;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
