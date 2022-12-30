#include "foobar.h"

#include <yt/yt/core/tracing/trace_context.h>

void StopHere() {
    volatile int dummy = 0;
    Y_UNUSED(dummy);
}

void AsyncStop(NYT::NConcurrency::IThreadPoolPtr& threadPool) {
    auto future = BIND([&]() {
        auto traceContext = NYT::NTracing::GetCurrentTraceContext();
        traceContext->AddTag("tag0", "value0");
        StopHere();
    }).AsyncVia(threadPool->GetInvoker()).Run();
    Y_UNUSED(NYT::NConcurrency::WaitFor(future));
}

int main() {
    auto traceContext = NYT::NTracing::CreateTraceContextFromCurrent("Test");
    traceContext->SetRecorded();
    traceContext->SetSampled();
    traceContext->AddTag("tag", "value");
    traceContext->SetLoggingTag("LoggingTag");
    NYT::NTracing::TTraceContextGuard guard(traceContext);
    auto threadPool = NYT::NConcurrency::CreateThreadPool(1, "test");
    auto future = BIND([&]() {
        Foo(threadPool, 10);
    }).AsyncVia(threadPool->GetInvoker()).Run();
    Y_UNUSED(NYT::NConcurrency::WaitFor(future));
    return 0;
}
