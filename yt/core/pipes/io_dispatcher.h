#pragma once

#include "public.h"

#include <core/misc/shutdownable.h>

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

class TIODispatcher
    : public IShutdownable
{
public:
    ~TIODispatcher();

    static TIODispatcher* Get();

    static void StaticShutdown();

    virtual void Shutdown() override;

private:
    TIODispatcher();

    DECLARE_SINGLETON_FRIEND();

    friend class NDetail::TAsyncReaderImpl;
    friend class NDetail::TAsyncWriterImpl;

    class TImpl;
    TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
