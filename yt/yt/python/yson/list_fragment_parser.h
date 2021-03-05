#pragma once

#include <yt/yt/python/common/stream.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/ref.h>

#include <yt/yt/core/yson/consumer.h>
#include <yt/yt/core/yson/lexer_detail.h>
#include <yt/yt/core/yson/parser.h>

#include <yt/yt/core/ytree/public.h>

#include <queue>
#include <stack>

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

class TListFragmentConsumer
    : public NYson::IYsonConsumer
{
public:
    explicit TListFragmentConsumer(TCallback<void()> checkItemCallback)
        : CheckItemCallback_(checkItemCallback)
    { }

    void OnListItem()
    {
        if (Balance_ == -1) {
            Balance_ = 0;
        } else {
            CheckItem();
        }
    }

    void OnKeyedItem(TStringBuf key)
    { }

    void OnBeginAttributes()
    {
        Balance_++;
    }

    void OnEndAttributes()
    {
        Balance_--;
    }

    void OnRaw(TStringBuf yson, NYson::EYsonType type)
    {
        YT_ABORT();
    }

    void OnStringScalar(TStringBuf /*value*/)
    { }

    void OnInt64Scalar(i64 /*value*/)
    { }

    void OnUint64Scalar(ui64 /*value*/)
    { }

    void OnDoubleScalar(double /*value*/)
    { }

    void OnBooleanScalar(bool /*value*/)
    { }

    void OnEntity()
    { }

    void OnBeginList()
    {
        Balance_++;
    }

    void OnEndList()
    {
        Balance_--;
    }

    void OnBeginMap()
    {
        Balance_++;
    }

    void OnEndMap()
    {
        Balance_--;
    }

private:
    void CheckItem()
    {
        if (Balance_ == 0) {
            CheckItemCallback_.Run();
        }
    }

    TCallback<void()> CheckItemCallback_;

    int Balance_ = -1;
};

////////////////////////////////////////////////////////////////////////////////

class TListFragmentParser
{
public:
    TListFragmentParser();
    explicit TListFragmentParser(IInputStream* stream);

    TSharedRef NextItem();

private:
    class TImpl;
    std::shared_ptr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
