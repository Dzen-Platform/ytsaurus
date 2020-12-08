#pragma once

#include "public.h"
#include "chunk_owner_base.h"

#include <yt/server/master/cypress_server/node_proxy_detail.h>

#include <yt/server/master/node_tracker_server/public.h>

#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <yt/client/chunk_client/read_limit.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

void BuildChunkSpec(
    TChunk* chunk,
    std::optional<i64> rowIndex,
    std::optional<int> tabletIndex,
    const NChunkClient::TLegacyReadLimit& lowerLimit,
    const NChunkClient::TLegacyReadLimit& upperLimit,
    TTransactionId timestampTransactionId,
    bool fetchParityReplicas,
    bool fetchAllMetaExtensions,
    const THashSet<int>& extensionTags,
    NNodeTrackerServer::TNodeDirectoryBuilder* nodeDirectoryBuilder,
    NCellMaster::TBootstrap* bootstrap,
    NChunkClient::NProto::TChunkSpec* chunkSpec);

void BuildDynamicStoreSpec(
    const TDynamicStore* dynamicStore,
    const NChunkClient::TLegacyReadLimit& lowerLimit,
    const NChunkClient::TLegacyReadLimit& upperLimit,
    NNodeTrackerServer::TNodeDirectoryBuilder* nodeDirectoryBuilder,
    NCellMaster::TBootstrap* bootstrap,
    NChunkClient::NProto::TChunkSpec* chunkSpec);

////////////////////////////////////////////////////////////////////////////////

class TChunkOwnerNodeProxy
    : public NCypressServer::TNontemplateCypressNodeProxyBase
{
public:
    TChunkOwnerNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TChunkOwnerBase* trunkNode);

    virtual NYTree::ENodeType GetType() const override;

protected:
    virtual void ListSystemAttributes(std::vector<NYTree::ISystemAttributeProvider::TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(NYTree::TInternedAttributeKey key, NYson::IYsonConsumer* consumer) override;
    virtual TFuture<NYson::TYsonString> GetBuiltinAttributeAsync(NYTree::TInternedAttributeKey key) override;

    virtual bool SetBuiltinAttribute(NYTree::TInternedAttributeKey key, const NYson::TYsonString& value) override;

    struct TFetchContext
    {
        NNodeTrackerClient::EAddressType AddressType = NNodeTrackerClient::EAddressType::InternalRpc;
        bool FetchParityReplicas = false;
        bool OmitDynamicStores = false;
        std::vector<NChunkClient::TLegacyReadRange> Ranges;
    };

    virtual bool DoInvoke(const NRpc::IServiceContextPtr& context) override;

    virtual void ValidateFetch(TFetchContext* context);

    void ValidateInUpdate();
    virtual void ValidateBeginUpload();
    virtual void ValidateStorageParametersUpdate() override;

    virtual void GetBasicAttributes(TGetBasicAttributesContext* context) override;

    DECLARE_YPATH_SERVICE_METHOD(NChunkClient::NProto, Fetch);
    DECLARE_YPATH_SERVICE_METHOD(NChunkClient::NProto, BeginUpload);
    DECLARE_YPATH_SERVICE_METHOD(NChunkClient::NProto, GetUploadParams);
    DECLARE_YPATH_SERVICE_METHOD(NChunkClient::NProto, EndUpload);

private:
    using TBase = NCypressServer::TNontemplateCypressNodeProxyBase;

    class TFetchChunkVisitor;

    void SetReplicationFactor(int replicationFactor);
    void SetVital(bool vital);
    void SetReplication(const TChunkReplication& replication);
    void SetPrimaryMedium(TMedium* medium);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
