#pragma once

#include "public.h"

#include <core/ytree/yson_serializable.h>

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

    TTcpBusConfig()
    {
        RegisterParameter("priority", Priority)
            .InRange(0, 6)
            .Default(0);
        RegisterParameter("enable_no_delay", EnableNoDelay)
            .Default(true);
        RegisterParameter("enable_quick_ack", EnableQuickAck)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TTcpBusConfig)

class TTcpBusServerConfig
    : public TTcpBusConfig
{
public:
    TNullable<int> Port;
    TNullable<Stroka> UnixDomainName;
    int MaxBacklogSize;
    int MaxNumberOfConnections;

    TTcpBusServerConfig()
    {
        RegisterParameter("port", Port)
            .Default();
        RegisterParameter("unix_domain_name", UnixDomainName)
            .Default();
        RegisterParameter("max_backlog_size", MaxBacklogSize)
            .Default(8192);
        RegisterParameter("max_number_of_connections", MaxNumberOfConnections)
            .Default(50000);
    }

    static TTcpBusServerConfigPtr CreateTcp(int port);

    static TTcpBusServerConfigPtr CreateUnixDomain(const Stroka& address);

};

DEFINE_REFCOUNTED_TYPE(TTcpBusServerConfig)

class TTcpBusClientConfig
    : public TTcpBusConfig
{
public:
    TNullable<Stroka> Address;
    TNullable<Stroka> UnixDomainName;

    TTcpBusClientConfig()
    {
        RegisterParameter("address", Address)
            .Default();
        RegisterParameter("unix_domain_name", UnixDomainName)
            .Default();

        RegisterValidator([&] () {
            if (!Address && !UnixDomainName) {
                THROW_ERROR_EXCEPTION("\"address\" and \"unix_domain_name\" cannot be both empty");
            }
        });
    }

    static TTcpBusClientConfigPtr CreateTcp(const Stroka& address);

    static TTcpBusClientConfigPtr CreateUnixDomain(const Stroka& address);

};

DEFINE_REFCOUNTED_TYPE(TTcpBusClientConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT

