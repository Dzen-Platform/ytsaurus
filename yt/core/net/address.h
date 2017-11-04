#pragma once

#include "public.h"

#include <yt/core/misc/error.h>

#include <yt/core/actions/future.h>

#include <yt/core/ytree/yson_serializable.h>

#ifdef _WIN32
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
#endif

#include <array>

namespace NYT {
namespace NNet {

////////////////////////////////////////////////////////////////////////////////

//! Constructs an address of the form |hostName:port|.
TString BuildServiceAddress(const TStringBuf& hostName, int port);

//! Parses service address into host name and port number.
//! Both #hostName and #port can be |NULL|.
//! Throws if the address is malformed.
void ParseServiceAddress(
    const TStringBuf& address,
    TStringBuf* hostName,
    int* port);

//! Extracts port number from a service address.
//! Throws if the address is malformed.
int GetServicePort(const TStringBuf& address);

//! Extracts host name from a service address.
TStringBuf GetServiceHostName(const TStringBuf& address);

////////////////////////////////////////////////////////////////////////////////

//! An opaque wrapper for |sockaddr| type.
class TNetworkAddress
{
public:
    TNetworkAddress();
    TNetworkAddress(const TNetworkAddress& other, int port);
    explicit TNetworkAddress(const sockaddr& other, socklen_t length = 0);
    TNetworkAddress(int family, const char* addr, size_t size);

    sockaddr* GetSockAddr();
    const sockaddr* GetSockAddr() const;
    socklen_t GetLength() const;
    socklen_t* GetLengthPtr();
    ui16 GetPort() const;

    static TErrorOr<TNetworkAddress> TryParse(const TStringBuf& address);
    static TNetworkAddress Parse(const TStringBuf& address);

    static TNetworkAddress CreateIPv6Any(int port);
    static TNetworkAddress CreateIPv6Loopback(int port);
    static TNetworkAddress CreateUnixDomainAddress(const TString& name);

private:
    sockaddr_storage Storage;
    socklen_t Length;

    static socklen_t GetGenericLength(const sockaddr& sockAddr);
};

TString ToString(const TNetworkAddress& address, bool withPort = true);

bool operator == (const TNetworkAddress& lhs, const TNetworkAddress& rhs);
bool operator != (const TNetworkAddress& lhs, const TNetworkAddress& rhs);

////////////////////////////////////////////////////////////////////////////////

class TIP6Address
{
public:
    static constexpr size_t ByteSize = 16;

    TIP6Address() = default;

    static TIP6Address FromString(const TStringBuf& str);
    static bool FromString(const TStringBuf& str, TIP6Address* address);

    static TIP6Address FromRawBytes(const ui8* raw);
    static TIP6Address FromRawWords(const ui16* raw);
    static TIP6Address FromRawDWords(const ui32* raw);

    const ui8* GetRawBytes() const;
    ui8* GetRawBytes();

    const ui16* GetRawWords() const;
    ui16* GetRawWords();

    const ui32* GetRawDWords() const;
    ui32* GetRawDWords();

private:
    std::array<ui8, ByteSize> Raw_ = {};
};

void FormatValue(TStringBuilder* builder, const TIP6Address& address, const TStringBuf& spec);
TString ToString(const TIP6Address& address);

bool operator == (const TIP6Address& lhs, const TIP6Address& rhs);
bool operator != (const TIP6Address& lhs, const TIP6Address& rhs);

TIP6Address operator & (const TIP6Address& lhs, const TIP6Address& rhs);
TIP6Address operator | (const TIP6Address& lhs, const TIP6Address& rhs);
TIP6Address& operator &= (TIP6Address& lhs, const TIP6Address& rhs);
TIP6Address& operator |= (TIP6Address& lhs, const TIP6Address& rhs);

////////////////////////////////////////////////////////////////////////////////

class TIP6Network
{
public:
    TIP6Network() = default;
    TIP6Network(const TIP6Address& network, const TIP6Address& mask);

    static TIP6Network FromString(const TStringBuf& str);
    static bool FromString(const TStringBuf& str, TIP6Network* network);

    bool Contains(const TIP6Address& address) const;

    const TIP6Address& GetNetwork() const;
    const TIP6Address& GetMask() const;
    int GetMaskSize() const;

private:
    TIP6Address Network_;
    TIP6Address Mask_;
};

void FormatValue(TStringBuilder* builder, const TIP6Network& network, const TStringBuf& spec);
TString ToString(const TIP6Network& network);

////////////////////////////////////////////////////////////////////////////////

//! Performs asynchronous host name resolution.
class TAddressResolver
{
public:
    ~TAddressResolver();

    //! Returns the singleton instance.
    static TAddressResolver* Get();

    //! Destroys the singleton instance.
    static void StaticShutdown();

    //! Shuts down all internals of address resolver.
    void Shutdown();

    //! Resolves #address asynchronously.
    /*!
     *  Calls |getaddrinfo| and returns the first entry belonging to |AF_INET| or |AF_INET6| family.
     *  Caches successful resolutions.
     */
    TFuture<TNetworkAddress> Resolve(const TString& address);

    //! Return |true| if the local host FQDN can be properly determined.
    bool IsLocalHostNameOK();

    //! Returns |true| if #address matches one of local host addresses.
    bool IsLocalAddress(const TNetworkAddress& address);

    //! Removes all cached resolutions.
    void PurgeCache();

    //! Updates resolver configuration.
    void Configure(TAddressResolverConfigPtr config);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

    TAddressResolver();

    Y_DECLARE_SINGLETON_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNet
} // namespace NYT
