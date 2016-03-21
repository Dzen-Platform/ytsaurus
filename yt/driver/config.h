#pragma once

#include <yt/ytlib/driver/config.h>

#include <yt/ytlib/formats/format.h>

#include <yt/core/misc/address.h>

#include <yt/core/ytree/helpers.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TFormatDefaultsConfig
    : public NYTree::TYsonSerializable
{
public:
    NFormats::TFormat Structured;
    NFormats::TFormat Tabular;

    TFormatDefaultsConfig()
    {
        // Keep this in sync with ytlib/driver/format.cpp
        auto structuredAttributes = NYTree::CreateEphemeralAttributes();
        structuredAttributes->Set("format", Stroka("pretty"));
        RegisterParameter("structured", Structured)
            .Default(NFormats::TFormat(NFormats::EFormatType::Yson, structuredAttributes.get()));

        auto tabularAttributes = NYTree::CreateEphemeralAttributes();
        tabularAttributes->Set("format", Stroka("text"));
        RegisterParameter("tabular", Tabular)
            .Default(NFormats::TFormat(NFormats::EFormatType::Yson, tabularAttributes.get()));
    }
};

typedef TIntrusivePtr<TFormatDefaultsConfig> TFormatDefaultsConfigPtr;

////////////////////////////////////////////////////////////////////////////////

class TExecutorConfig
    : public NYTree::TYsonSerializable
{
public:
    NDriver::TDriverConfigPtr Driver;
    NYTree::INodePtr Logging;
    NYTree::INodePtr Tracing;
    TAddressResolverConfigPtr AddressResolver;
    TFormatDefaultsConfigPtr FormatDefaults;
    TDuration OperationPollPeriod;
    bool Trace;

    TExecutorConfig()
    {
        RegisterParameter("driver", Driver);
        RegisterParameter("logging", Logging)
            .Default();
        RegisterParameter("tracing", Tracing)
            .Default();
        RegisterParameter("address_resolver", AddressResolver)
            .DefaultNew();
        RegisterParameter("format_defaults", FormatDefaults)
            .DefaultNew();
        RegisterParameter("operation_poll_period", OperationPollPeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("trace", Trace)
            .Default(false);
    }
};

typedef TIntrusivePtr<TExecutorConfig> TExecutorConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
