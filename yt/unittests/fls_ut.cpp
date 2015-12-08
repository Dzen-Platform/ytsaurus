#include "framework.h"

#include <yt/core/concurrency/fls.h>

namespace NYT {
namespace NConcurrency {
namespace {

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TMyValue
{
    static int CtorCalls;
    static int DtorCalls;

    T Value;

    static void Reset()
    {
        CtorCalls = 0;
        DtorCalls = 0;
    }

    TMyValue()
    {
        ++CtorCalls;
    }

    ~TMyValue()
    {
        ++DtorCalls;
    }
};

template <> int TMyValue<int>::CtorCalls = 0;
template <> int TMyValue<int>::DtorCalls = 0;

template <> int TMyValue<Stroka>::CtorCalls = 0;
template <> int TMyValue<Stroka>::DtorCalls = 0;

class TFlsTest
    : public ::testing::Test
{
protected:
    virtual void SetUp()
    {
        TMyValue<int>::Reset();
        TMyValue<Stroka>::Reset();
    }

};

TFls<TMyValue<int>> IntValue;
TFls<TMyValue<Stroka>> StringValue;

#if 0

TEST_F(TFlsTest, OneFiber)
{
    auto fiber = New<TFiber>(BIND([] () {
        EXPECT_EQ(0, TMyValue<int>::CtorCalls);
        IntValue->Value = 1;
        EXPECT_EQ(1, TMyValue<int>::CtorCalls);
    }));

    fiber->Run();
    EXPECT_EQ(EFiberState::Terminated, fiber->GetState());

    fiber.Reset();

    EXPECT_EQ(1, TMyValue<int>::CtorCalls);
    EXPECT_EQ(1, TMyValue<int>::DtorCalls);
}

TEST_F(TFlsTest, TwoFibers)
{
    auto fiber1 = New<TFiber>(BIND([] () {
        EXPECT_EQ(0, TMyValue<Stroka>::CtorCalls);
        StringValue->Value = "fiber1";
        EXPECT_EQ(1, TMyValue<Stroka>::CtorCalls);

        Yield();

        EXPECT_EQ("fiber1", StringValue->Value);
    }));

    auto fiber2 = New<TFiber>(BIND([] () {
        EXPECT_EQ(1, TMyValue<Stroka>::CtorCalls);
        StringValue->Value = "fiber2";
        EXPECT_EQ(2, TMyValue<Stroka>::CtorCalls);

        Yield();

        EXPECT_EQ("fiber2", StringValue->Value);
    }));

    fiber1->Run();
    EXPECT_EQ(EFiberState::Suspended, fiber1->GetState());

    EXPECT_EQ(1, TMyValue<Stroka>::CtorCalls);
    EXPECT_EQ(0, TMyValue<int>::DtorCalls);

    fiber2->Run();
    EXPECT_EQ(EFiberState::Suspended, fiber2->GetState());

    EXPECT_EQ(2, TMyValue<Stroka>::CtorCalls);
    EXPECT_EQ(0, TMyValue<Stroka>::DtorCalls);

    fiber1->Run();
    EXPECT_EQ(EFiberState::Terminated, fiber1->GetState());

    EXPECT_EQ(2, TMyValue<Stroka>::CtorCalls);
    EXPECT_EQ(0, TMyValue<Stroka>::DtorCalls);

    fiber2->Run();
    EXPECT_EQ(EFiberState::Terminated, fiber2->GetState());

    fiber1.Reset();
    fiber2.Reset();

    EXPECT_EQ(2, TMyValue<Stroka>::CtorCalls);
    EXPECT_EQ(2, TMyValue<Stroka>::DtorCalls);
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NConcurrency
} // namespace NYT

