#pragma once

#include "public.h"

#include <core/yson/writer.h>
#include <core/yson/forwarding_consumer.h>
#include <core/yson/stream.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TAttributeConsumer
    : public NYson::TForwardingYsonConsumer
{
public:
    explicit TAttributeConsumer(IAttributeDictionary* attributes);
    IAttributeDictionary* GetAttributes() const;

protected:
    virtual void OnMyStringScalar(const TStringBuf& value) override;
    virtual void OnMyInt64Scalar(i64 value) override;
    virtual void OnMyUint64Scalar(ui64 value) override;
    virtual void OnMyDoubleScalar(double value) override;
    virtual void OnMyBooleanScalar(bool value) override;
    virtual void OnMyEntity() override;
    virtual void OnMyBeginList() override;

    virtual void OnMyKeyedItem(const TStringBuf& key) override;
    virtual void OnMyBeginMap() override;
    virtual void OnMyEndMap() override;
    virtual void OnMyBeginAttributes() override;
    virtual void OnMyEndAttributes()override;

private:
    IAttributeDictionary* Attributes;
    TStringStream Output;
    std::unique_ptr<NYson::TYsonWriter> Writer;

    void ThrowMapExpected();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
