#pragma once

#include "public.h"

#include <yt/core/misc/shutdownable.h>

#include <yt/core/ytree/public.h>

#include <util/generic/singleton.h>

namespace NYT {
namespace NTracing {

////////////////////////////////////////////////////////////////////////////////

class TTraceManager
    : public IShutdownable
{
public:
    ~TTraceManager();

    static TTraceManager* Get();

    static void StaticShutdown();

    void Configure(NYTree::INodePtr node, const NYPath::TYPath& path = "");
    void Configure(const Stroka& fileName, const NYPath::TYPath& path);

    virtual void Shutdown() override;

    void Enqueue(
        const NTracing::TTraceContext& context,
        const Stroka& serviceName,
        const Stroka& spanName,
        const Stroka& annotationName);

    void Enqueue(
        const NTracing::TTraceContext& context,
        const Stroka& annotationKey,
        const Stroka& annotationValue);

private:
    TTraceManager();

    Y_DECLARE_SINGLETON_FRIEND();

    class TImpl;
    std::unique_ptr<TImpl> Impl_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTracing
} // namespace NYT

