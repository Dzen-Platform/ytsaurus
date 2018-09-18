#pragma once

#include <yt/core/yson/consumer.h>

#include <contrib/libs/gmock/gmock/gmock.h>

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

class TMockYsonConsumer
    : public TYsonConsumerBase
{
public:
    MOCK_METHOD1(OnStringScalar, void(TStringBuf value));
    MOCK_METHOD1(OnInt64Scalar, void(i64 value));
    MOCK_METHOD1(OnUint64Scalar, void(ui64 value));
    MOCK_METHOD1(OnDoubleScalar, void(double value));
    MOCK_METHOD1(OnBooleanScalar, void(bool value));
    MOCK_METHOD0(OnEntity, void());

    MOCK_METHOD0(OnBeginList, void());
    MOCK_METHOD0(OnListItem, void());
    MOCK_METHOD0(OnEndList, void());

    MOCK_METHOD0(OnBeginMap, void());
    MOCK_METHOD1(OnKeyedItem, void(TStringBuf name));
    MOCK_METHOD0(OnEndMap, void());

    MOCK_METHOD0(OnBeginAttributes, void());
    MOCK_METHOD0(OnEndAttributes, void());
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
