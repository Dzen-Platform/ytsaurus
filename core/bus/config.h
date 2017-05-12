#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

class TTcpBusConfig
    : public NYTree::TYsonSerializable
{
public:
    int Priority;
    bool EnableNoDelay;
    bool EnableQuickAck;

    int BindRetryCount;
    TDuration BindRetryBackoff;

    TDuration ReadStallTimeout;
    TDuration WriteStallTimeout;

    bool VerifyChecksum;
    bool CalculateChecksum;

    TTcpBusConfig()
    {
        RegisterParameter("priority", Priority)
            .InRange(0, 6)
            .Default(0);
        RegisterParameter("enable_no_delay", EnableNoDelay)
            .Default(true);
        RegisterParameter("enable_quick_ack", EnableQuickAck)
            .Default(true);
        RegisterParameter("bind_retry_count", BindRetryCount)
            .Default(1);
        RegisterParameter("bind_retry_backoff", BindRetryBackoff)
            .Default(TDuration::Seconds(3));
        RegisterParameter("read_stall_timeout", ReadStallTimeout)
            .Default(TDuration::Minutes(2));
        RegisterParameter("write_stall_timeout", WriteStallTimeout)
            .Default(TDuration::Minutes(2));
        RegisterParameter("verify_checksum", VerifyChecksum)
            .Default(true);
        RegisterParameter("calculate_checksum", CalculateChecksum)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TTcpBusConfig)

class TTcpBusServerConfig
    : public TTcpBusConfig
{
public:
    TNullable<int> Port;
    TNullable<TString> UnixDomainName;
    int MaxBacklogSize;
    int MaxSimultaneousConnections;

    TTcpBusServerConfig()
    {
        RegisterParameter("port", Port)
            .Default();
        RegisterParameter("unix_domain_name", UnixDomainName)
            .Default();
        RegisterParameter("max_backlog_size", MaxBacklogSize)
            .Default(8192);
        RegisterParameter("max_simultaneous_connections", MaxSimultaneousConnections)
            .Default(50000);
    }

    static TTcpBusServerConfigPtr CreateTcp(int port);

    static TTcpBusServerConfigPtr CreateUnixDomain(const TString& address);

};

DEFINE_REFCOUNTED_TYPE(TTcpBusServerConfig)

class TTcpBusClientConfig
    : public TTcpBusConfig
{
public:
    TNullable<TString> Address;
    TNullable<TString> UnixDomainName;

    TTcpBusClientConfig()
    {
        RegisterParameter("address", Address)
            .Default();
        RegisterParameter("unix_domain_name", UnixDomainName)
            .Default();

        RegisterValidator([&] () {
            if (!Address && !UnixDomainName) {
                THROW_ERROR_EXCEPTION("\"address\" and \"unix_domain_name\" cannot be both missing");
            }
        });
    }

    static TTcpBusClientConfigPtr CreateTcp(const TString& address);

    static TTcpBusClientConfigPtr CreateUnixDomain(const TString& address);

};

DEFINE_REFCOUNTED_TYPE(TTcpBusClientConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT

