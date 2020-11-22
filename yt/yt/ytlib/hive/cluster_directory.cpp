#include "public.h"
#include "cluster_directory.h"
#include "private.h"

#include <yt/client/hive/proto/cluster_directory.pb.h>

#include <yt/ytlib/api/connection.h>

#include <yt/client/api/connection.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/collection_helpers.h>

#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/convert.h>

namespace NYT::NHiveClient {

using namespace NRpc;
using namespace NApi;
using namespace NObjectClient;
using namespace NYTree;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = HiveClientLogger;

////////////////////////////////////////////////////////////////////////////////

IConnectionPtr TClusterDirectory::FindConnection(TCellTag cellTag) const
{
    auto guard = Guard(Lock_);
    auto it = CellTagToCluster_.find(cellTag);
    return it == CellTagToCluster_.end() ? nullptr : it->second.Connection;
}

IConnectionPtr TClusterDirectory::GetConnectionOrThrow(TCellTag cellTag) const
{
    auto connection = FindConnection(cellTag);
    if (!connection) {
        THROW_ERROR_EXCEPTION("Cannot find cluster with cell tag %v", cellTag);
    }
    return connection;
}

IConnectionPtr TClusterDirectory::FindConnection(const TString& clusterName) const
{
    auto guard = Guard(Lock_);
    auto it = NameToCluster_.find(clusterName);
    return it == NameToCluster_.end() ? nullptr : it->second.Connection;
}

IConnectionPtr TClusterDirectory::GetConnectionOrThrow(const TString& clusterName) const
{
    auto connection = FindConnection(clusterName);
    if (!connection) {
        THROW_ERROR_EXCEPTION("Cannot find cluster with name %Qv", clusterName);
    }
    return connection;
}

std::vector<TString> TClusterDirectory::GetClusterNames() const
{
    auto guard = Guard(Lock_);
    return GetKeys(NameToCluster_);
}

void TClusterDirectory::RemoveCluster(const TString& name)
{
    auto guard = Guard(Lock_);
    auto it = NameToCluster_.find(name);
    if (it == NameToCluster_.end()) {
        return;
    }
    const auto& cluster = it->second;
    auto cellTag = GetCellTag(cluster);
    cluster.Connection->Terminate();
    NameToCluster_.erase(it);
    YT_VERIFY(CellTagToCluster_.erase(cellTag) == 1);
    YT_LOG_DEBUG("Remote cluster unregistered (Name: %v)",
        name);
}

void TClusterDirectory::Clear()
{
    auto guard = Guard(Lock_);
    CellTagToCluster_.clear();
    NameToCluster_.clear();
}

void TClusterDirectory::UpdateCluster(const TString& name, INodePtr config)
{
    auto addNewCluster = [&] (const TCluster& cluster) {
        auto cellTag = GetCellTag(cluster);
        if (CellTagToCluster_.find(cellTag) != CellTagToCluster_.end()) {
            THROW_ERROR_EXCEPTION("Duplicate cell tag %v", cellTag);
        }
        CellTagToCluster_[cellTag] = cluster;
        NameToCluster_[name] = cluster;
    };

    auto it = NameToCluster_.find(name);
    if (it == NameToCluster_.end()) {
        auto cluster = CreateCluster(name, config);
        auto guard = Guard(Lock_);
        addNewCluster(cluster);
        YT_LOG_DEBUG("Remote cluster registered (Name: %v, CellTag: %v)",
            name,
            cluster.Connection->GetCellTag());
    } else if (!AreNodesEqual(it->second.Config, config)) {
        auto cluster = CreateCluster(name, config);
        auto guard = Guard(Lock_);
        it->second.Connection->Terminate();
        CellTagToCluster_.erase(GetCellTag(it->second));
        NameToCluster_.erase(it);
        addNewCluster(cluster);
        YT_LOG_DEBUG("Remote cluster updated (Name: %v, CellTag: %v)",
            name,
            cluster.Connection->GetCellTag());
    }
}

void TClusterDirectory::UpdateDirectory(const NProto::TClusterDirectory& protoDirectory)
{
    THashMap<TString, INodePtr> nameToConfig;
    for (const auto& item : protoDirectory.items()) {
        YT_VERIFY(nameToConfig.emplace(
            item.name(),
            ConvertToNode(NYson::TYsonString(item.config()))).second);
    }

    for (const auto& name : GetClusterNames()) {
        if (nameToConfig.find(name) == nameToConfig.end()) {
            RemoveCluster(name);
        }
    }

    for (const auto& [name, config] : nameToConfig) {
        UpdateCluster(name, config);
    }
}

TClusterDirectory::TCluster TClusterDirectory::CreateCluster(const TString& name, INodePtr config) const
{
    TCluster cluster;
    cluster.Config = config;
    try {
        cluster.Connection = CreateConnection(config);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error creating connection to cluster %Qv",
            name)
            << ex;
    }
    return cluster;
}

TCellTag TClusterDirectory::GetCellTag(const TClusterDirectory::TCluster& cluster)
{
    return cluster.Connection->GetCellTag();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveClient

