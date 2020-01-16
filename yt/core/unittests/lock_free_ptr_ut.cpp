#include <yt/core/test_framework/framework.h>

#include <yt/core/misc/format.h>
#include <yt/core/misc/new.h>
#include <yt/core/misc/public.h>
#include <yt/core/misc/atomic_ptr.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

class TSampleObject
{
public:
    explicit TSampleObject(IOutputStream* output)
        : Output_(output)
    {
        *Output_ << 'C';
    }

    ~TSampleObject()
    {
        *Output_ << 'D';
    }

    void DoSomething()
    {
        *Output_ << '!';
    }

private:
    IOutputStream* const Output_;

};

class TTestAllocator
{
public:
    explicit TTestAllocator(IOutputStream* output)
        : Output_(output)
    { }

    void* Allocate(size_t size)
    {
        *Output_ << 'A';
        ++AllocatedCount_;

        size += sizeof(void*);
        auto ptr = NYTAlloc::Allocate(size);
        auto* header = static_cast<TTestAllocator**>(ptr);
        *header = this;
        return header + 1;
    }

    static void Free(void* ptr)
    {
        auto* header = static_cast<TTestAllocator**>(ptr) - 1;
        auto* allocator = *header;

        *allocator->Output_ << 'F';
        ++allocator->DeallocatedCount_;

        NYTAlloc::Free(header);
    }

    ~TTestAllocator()
    {
        YT_VERIFY(AllocatedCount_ == DeallocatedCount_);
    }

private:
    IOutputStream* const Output_;
    size_t AllocatedCount_ = 0;
    size_t DeallocatedCount_ = 0;
};

TEST(TLockFreePtrTest, RefCountedPtrBehavior)
{
    TStringStream output;
    TTestAllocator allocator(&output);

    {
        auto ptr = CreateObjectWithExtraSpace<TSampleObject>(&allocator, 0, &output);
        {
            auto anotherPtr = ptr;
            anotherPtr->DoSomething();
        }
        {
            auto anotherPtr = ptr;
            anotherPtr->DoSomething();
        }
        ptr->DoSomething();
    }

    EXPECT_STREQ("AC!!!D", output.Str().c_str());

    ScanDeleteList();

    EXPECT_STREQ("AC!!!DF", output.Str().c_str());
}

TEST(TLockFreePtrTest, DelayedDeallocation)
{
    TStringStream output;
    TTestAllocator allocator(&output);

    auto ptr = CreateObjectWithExtraSpace<TSampleObject>(&allocator, 0, &output);
    ptr->DoSomething();

    auto hazardPtr = THazardPtr<TSampleObject>::Acquire([&] {
        return ptr.Get();
    });

    ptr = nullptr;

    EXPECT_STREQ("AC!D", output.Str().c_str());

    ScanDeleteList();

    EXPECT_STREQ("AC!D", output.Str().c_str());

    hazardPtr.Reset();
    ScanDeleteList();

    EXPECT_STREQ("AC!DF", output.Str().c_str());
}

TEST(TLockFreePtrTest, CombinedLogic)
{
    TStringStream output;
    TTestAllocator allocator(&output);

    auto ptr = CreateObjectWithExtraSpace<TSampleObject>(&allocator, 0, &output);
    ptr->DoSomething();

    auto ptrCopy = ptr;
    auto rawPtr = ptrCopy.Release();

    auto hazardPtr = THazardPtr<TSampleObject>::Acquire([&] {
        return ptr.Get();
    });

    ptr = nullptr;

    EXPECT_STREQ("AC!", output.Str().c_str());

    ScheduleObjectDeletion(rawPtr, [] (void* ptr) {
        ReleaseRef<TTestAllocator>(reinterpret_cast<TSampleObject*>(ptr));
    });

    ScanDeleteList();

    EXPECT_STREQ("AC!", output.Str().c_str());

    {
        hazardPtr.Reset();
        ScanDeleteList();

        EXPECT_STREQ("AC!D", output.Str().c_str());
    }

    {
        auto hazardPtr = THazardPtr<TSampleObject>::Acquire([&] {
            return rawPtr;
        });

        ScanDeleteList();
        EXPECT_STREQ("AC!D", output.Str().c_str());
    }

    {
        ScanDeleteList();
        EXPECT_STREQ("AC!DF", output.Str().c_str());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
