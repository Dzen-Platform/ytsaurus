#pragma once

#include "common.h"

#include <ytlib/driver/public.h>

#include <core/ytree/public.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

class TDriverWrap
    : public node::ObjectWrap
{
protected:
    TDriverWrap(bool echo, v8::Handle<v8::Object> configObject);
    ~TDriverWrap();

public:
    using node::ObjectWrap::Ref;
    using node::ObjectWrap::Unref;

    static v8::Persistent<v8::FunctionTemplate> ConstructorTemplate;
    static void Initialize(v8::Handle<v8::Object> target);
    static bool HasInstance(v8::Handle<v8::Value> value);

    // Synchronous JS API.
    static v8::Handle<v8::Value> New(const v8::Arguments& args);

    static v8::Handle<v8::Value> FindCommandDescriptor(const v8::Arguments& args);
    v8::Handle<v8::Value> DoFindCommandDescriptor(const Stroka& commandName);

    static v8::Handle<v8::Value> GetCommandDescriptors(const v8::Arguments& args);
    v8::Handle<v8::Value> DoGetCommandDescriptors();

    // Asynchronous JS API.
    static v8::Handle<v8::Value> Execute(const v8::Arguments& args);
    static void ExecuteWork(uv_work_t* workRequest);
    static void ExecuteAfter(uv_work_t* workRequest);

private:
    NDriver::IDriverPtr Driver;
    Stroka Message;

    // This is for testing purposes only.
    const bool Echo;

private:
    TDriverWrap(const TDriverWrap&);
    TDriverWrap& operator=(const TDriverWrap&);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT

