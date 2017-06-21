#include "node_directory.h"

#include <yt/ytlib/node_tracker_client/node_directory.pb.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/string.h>

namespace NYT {
namespace NNodeTrackerClient {

using namespace NChunkClient;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const TString NullAddress("<Null>");

////////////////////////////////////////////////////////////////////////////////

TNodeDescriptor::TNodeDescriptor()
    : DefaultAddress_(NullAddress)
{ }

TNodeDescriptor::TNodeDescriptor(const TString& defaultAddress)
    : Addresses_{std::make_pair(DefaultNetworkName, defaultAddress)}
    , DefaultAddress_(defaultAddress)
{ }

TNodeDescriptor::TNodeDescriptor(const TNullable<TString>& defaultAddress)
{
    if (defaultAddress) {
        *this = TNodeDescriptor(*defaultAddress);
    }
}

TNodeDescriptor::TNodeDescriptor(
    TAddressMap addresses,
    TNullable<TString> rack,
    TNullable<TString> dc)
    : Addresses_(std::move(addresses))
    , DefaultAddress_(NNodeTrackerClient::GetDefaultAddress(Addresses_))
    , Rack_(std::move(rack))
    , DataCenter_(std::move(dc))
{ }

bool TNodeDescriptor::IsNull() const
{
    return Addresses_.empty();
}

const TAddressMap& TNodeDescriptor::Addresses() const
{
    return Addresses_;
}

const TString& TNodeDescriptor::GetDefaultAddress() const
{
    return DefaultAddress_;
}

const TString& TNodeDescriptor::GetAddress(const TNetworkPreferenceList& networks) const
{
    return NNodeTrackerClient::GetAddress(Addresses(), networks);
}

TNullable<TString> TNodeDescriptor::FindAddress(const TNetworkPreferenceList& networks) const
{
    return NNodeTrackerClient::FindAddress(Addresses(), networks);
}

const TNullable<TString>& TNodeDescriptor::GetRack() const
{
    return Rack_;
}

const TNullable<TString>& TNodeDescriptor::GetDataCenter() const
{
    return DataCenter_;
}

void TNodeDescriptor::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Addresses_);
    if (context.IsLoad()) {
        DefaultAddress_ = NNodeTrackerClient::GetDefaultAddress(Addresses_);
    }
    Persist(context, Rack_);
    Persist(context, DataCenter_);
}

void FormatValue(TStringBuilder* builder, const TNodeDescriptor& descriptor, const TStringBuf& /*spec*/)
{
    if (descriptor.IsNull()) {
        builder->AppendString(NullAddress);
        return;
    }

    builder->AppendString(descriptor.GetDefaultAddress());
    if (const auto& rack = descriptor.GetRack()) {
        builder->AppendChar('@');
        builder->AppendString(*rack);
    }
    if (const auto& dataCenter = descriptor.GetDataCenter()) {
        builder->AppendChar('#');
        builder->AppendString(*dataCenter);
    }
}

TString ToString(const TNodeDescriptor& descriptor)
{
    return ToStringViaBuilder(descriptor);
}

const TString& GetDefaultAddress(const TAddressMap& addresses)
{
    if (addresses.empty()) {
        return NullAddress;
    }
    auto it = addresses.find(DefaultNetworkName);
    YCHECK(it != addresses.end());
    return it->second;
}

const TString& GetDefaultAddress(const NProto::TAddressMap& addresses)
{
    if (addresses.entries_size() == 0) {
        return NullAddress;
    }
    for (const auto& entry : addresses.entries()) {
        if (entry.network() == DefaultNetworkName) {
            return entry.address();
        }
    }
    Y_UNREACHABLE();
}

EAddressLocality ComputeAddressLocality(const TNodeDescriptor& first, const TNodeDescriptor& second)
{
    if (first.IsNull() || second.IsNull()) {
        return EAddressLocality::None;
    };

    try {
        if (GetServiceHostName(first.GetDefaultAddress()) == GetServiceHostName(second.GetDefaultAddress())) {
            return EAddressLocality::SameHost;
        }

        if (first.GetRack() && second.GetRack() && *first.GetRack() == *second.GetRack()) {
            return EAddressLocality::SameRack;
        }

        if (first.GetDataCenter() && second.GetDataCenter() && *first.GetDataCenter() == *second.GetDataCenter()) {
            return EAddressLocality::SameDataCenter;
        }
    } catch (const std::exception&) {
        // If one of the descriptors is malformed, treat it as None locality and ignore errors.
    }

    return EAddressLocality::None;
}

namespace NProto {

void ToProto(NNodeTrackerClient::NProto::TAddressMap* protoAddresses, const NNodeTrackerClient::TAddressMap& addresses)
{
    for (const auto& pair : addresses) {
        auto* entry = protoAddresses->add_entries();
        entry->set_network(pair.first);
        entry->set_address(pair.second);
    }
}

void FromProto(NNodeTrackerClient::TAddressMap* addresses, const NNodeTrackerClient::NProto::TAddressMap& protoAddresses)
{
    addresses->clear();
    addresses->reserve(protoAddresses.entries_size());
    for (const auto& entry : protoAddresses.entries()) {
        YCHECK(addresses->insert(std::make_pair(entry.network(), entry.address())).second);
    }
}

void ToProto(NNodeTrackerClient::NProto::TNodeDescriptor* protoDescriptor, const NNodeTrackerClient::TNodeDescriptor& descriptor)
{
    using NYT::ToProto;

    ToProto(protoDescriptor->mutable_addresses(), descriptor.Addresses());

    if (descriptor.GetRack()) {
        protoDescriptor->set_rack(*descriptor.GetRack());
    } else {
        protoDescriptor->clear_rack();
    }

    if (descriptor.GetDataCenter()) {
        protoDescriptor->set_data_center(*descriptor.GetDataCenter());
    } else {
        protoDescriptor->clear_data_center();
    }
}

void FromProto(NNodeTrackerClient::TNodeDescriptor* descriptor, const NNodeTrackerClient::NProto::TNodeDescriptor& protoDescriptor)
{
    using NYT::FromProto;

    *descriptor = NNodeTrackerClient::TNodeDescriptor(
        FromProto<NNodeTrackerClient::TAddressMap>(protoDescriptor.addresses()),
        protoDescriptor.has_rack() ? MakeNullable(protoDescriptor.rack()) : Null,
        protoDescriptor.has_data_center() ? MakeNullable(protoDescriptor.data_center()) : Null);
}

} // namespace NProto

bool operator == (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs)
{
    return
        lhs.GetDefaultAddress() == rhs.GetDefaultAddress() &&
        lhs.GetRack() == rhs.GetRack() &&
        lhs.GetDataCenter() == rhs.GetDataCenter();
}

bool operator != (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs)
{
    return !(lhs == rhs);
}

bool operator == (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs)
{
    if (lhs.GetDefaultAddress() != GetDefaultAddress(rhs.addresses())) {
        return false;
    }

    const auto& lhsMaybeRack = lhs.GetRack();
    auto lhsRack = lhsMaybeRack ? TStringBuf(*lhsMaybeRack) : TStringBuf();
    if (lhsRack != rhs.rack()) {
        return false;
    }

    const auto& lhsMaybeDataCenter= lhs.GetDataCenter();
    auto lhsDataCenter = lhsMaybeDataCenter ? TStringBuf(*lhsMaybeDataCenter) : TStringBuf();
    if (lhsDataCenter != rhs.data_center()) {
        return false;
    }

    return true;
}

bool operator != (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void TNodeDirectory::MergeFrom(const NProto::TNodeDirectory& source)
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    for (const auto& item : source.items()) {
        DoAddDescriptor(item.node_id(), item.node_descriptor());
    }
}

void TNodeDirectory::MergeFrom(const TNodeDirectoryPtr& source)
{
    if (this == source.Get()) {
        return;
    }
    NConcurrency::TWriterGuard thisGuard(SpinLock_);
    NConcurrency::TReaderGuard sourceGuard(source->SpinLock_);
    for (const auto& pair : source->IdToDescriptor_) {
        DoAddDescriptor(pair.first, *pair.second);
    }
}

void TNodeDirectory::DumpTo(NProto::TNodeDirectory* destination)
{
    NConcurrency::TReaderGuard guard(SpinLock_);
    for (const auto& pair : IdToDescriptor_) {
        auto* item = destination->add_items();
        item->set_node_id(pair.first);
        ToProto(item->mutable_node_descriptor(), *pair.second);
    }
}

void TNodeDirectory::AddDescriptor(TNodeId id, const TNodeDescriptor& descriptor)
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    DoAddDescriptor(id, descriptor);
}

void TNodeDirectory::DoAddDescriptor(TNodeId id, const TNodeDescriptor& descriptor)
{
    auto it = IdToDescriptor_.find(id);
    if (it != IdToDescriptor_.end() && *it->second == descriptor) {
        return;
    }

    auto descriptorHolder = std::make_unique<TNodeDescriptor>(descriptor);
    DoAddCapturedDescriptor(id, std::move(descriptorHolder));
}

void TNodeDirectory::DoAddDescriptor(TNodeId id, const NProto::TNodeDescriptor& protoDescriptor)
{
    auto it = IdToDescriptor_.find(id);
    if (it != IdToDescriptor_.end() && *it->second == protoDescriptor) {
        return;
    }

    auto descriptorHolder = std::make_unique<TNodeDescriptor>();
    FromProto(descriptorHolder.get(), protoDescriptor);
    DoAddCapturedDescriptor(id, std::move(descriptorHolder));
}

void TNodeDirectory::DoAddCapturedDescriptor(TNodeId id, std::unique_ptr<TNodeDescriptor> descriptorHolder)
{
    auto* capturedDescriptor = descriptorHolder.get();
    Descriptors_.emplace_back(std::move(descriptorHolder));
    IdToDescriptor_[id] = capturedDescriptor;
    AddressToDescriptor_[capturedDescriptor->GetDefaultAddress()] = capturedDescriptor;
}

const TNodeDescriptor* TNodeDirectory::FindDescriptor(TNodeId id) const
{
    NConcurrency::TReaderGuard guard(SpinLock_);
    auto it = IdToDescriptor_.find(id);
    return it == IdToDescriptor_.end() ? nullptr : it->second;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(TNodeId id) const
{
    const auto* result = FindDescriptor(id);
    YCHECK(result);
    return *result;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(TChunkReplica replica) const
{
    return GetDescriptor(replica.GetNodeId());
}

std::vector<TNodeDescriptor> TNodeDirectory::GetDescriptors(const TChunkReplicaList& replicas) const
{
    std::vector<TNodeDescriptor> result;
    for (auto replica : replicas) {
        result.push_back(GetDescriptor(replica));
    }
    return result;
}

const TNodeDescriptor* TNodeDirectory::FindDescriptor(const TString& address)
{
    NConcurrency::TReaderGuard guard(SpinLock_);
    auto it = AddressToDescriptor_.find(address);
    return it == AddressToDescriptor_.end() ? nullptr : it->second;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(const TString& address)
{
    const auto* result = FindDescriptor(address);
    YCHECK(result);
    return *result;
}

void TNodeDirectory::Save(TStreamSaveContext& context) const
{
    yhash<TNodeId, TNodeDescriptor> idToDescriptor;
    {
        NConcurrency::TReaderGuard guard(SpinLock_);
        for (const auto& pair : IdToDescriptor_) {
            YCHECK(idToDescriptor.emplace(pair.first, *pair.second).second);
        }
    }
    using NYT::Save;
    Save(context, idToDescriptor);
}

void TNodeDirectory::Load(TStreamLoadContext& context)
{
    using NYT::Load;
    auto idToDescriptor = Load<yhash<TNodeId, TNodeDescriptor>>(context);
    NConcurrency::TWriterGuard guard(SpinLock_);
    for (const auto& pair : idToDescriptor) {
        DoAddDescriptor(pair.first, pair.second);
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace {

TAddressMap::const_iterator SelectAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    for (const auto& network : networks) {
        const auto it = addresses.find(network);
        if (it != addresses.cend()) {
            return it;
        }
    }

    return addresses.cend();
}

} // namespace

TNullable<TString> FindAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    const auto it = SelectAddress(addresses, networks);
    return it == addresses.cend() ? Null : MakeNullable(it->second);
}

const TString& GetAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    const auto it = SelectAddress(addresses, networks);
    if (it != addresses.cend()) {
        return it->second;
    }

    THROW_ERROR_EXCEPTION("Cannot select address for host %v since there is no compatible network",
        GetDefaultAddress(addresses))
        << TErrorAttribute("remote_networks", GetKeys(addresses))
        << TErrorAttribute("local_networks", networks);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT

