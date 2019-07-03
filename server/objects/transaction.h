#pragma once

#include "persistence.h"

#include <yt/client/api/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/ypath/public.h>

#include <yt/core/misc/variant.h>

#include <yt/core/concurrency/async_semaphore.h>
#include <yp/client/api/proto/object_service.pb.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

struct TTransactionCommitResult
{
    TTimestamp CommitTimestamp;
};

////////////////////////////////////////////////////////////////////////////////

struct TSetUpdateRequest
{
    NYT::NYPath::TYPath Path;
    NYT::NYTree::INodePtr Value;
    bool Recursive = false;
};

struct TRemoveUpdateRequest
{
    NYT::NYPath::TYPath Path;
};

using TUpdateRequest = std::variant<
    TSetUpdateRequest,
    TRemoveUpdateRequest
>;

void FromProto(
    TRemoveUpdateRequest* request,
    const NClient::NApi::NProto::TRemoveUpdate& protoRequest);

////////////////////////////////////////////////////////////////////////////////

struct TAttributeTimestampPrerequisite
{
    NYT::NYPath::TYPath Path;
    TTimestamp Timestamp;
};

void FromProto(
    TAttributeTimestampPrerequisite* prerequisite,
    const NClient::NApi::NProto::TAttributeTimestampPrerequisite& protoPrerequisite);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETransactionState,
    (Active)
    (Committing)
    (Committed)
    (Failed)
    (Aborted)
);

////////////////////////////////////////////////////////////////////////////////

struct IUpdateContext
{
    virtual ~IUpdateContext() = default;

    virtual void AddSetter(std::function<void()> setter) = 0;
    virtual void AddFinalizer(std::function<void()> finalizer) = 0;

    virtual void Commit() = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct TAttributeValueList
{
    std::vector<NYT::NYson::TYsonString> Values;
    std::vector<TTimestamp> Timestamps;
};

////////////////////////////////////////////////////////////////////////////////

struct TAttributeSelector
{
    std::vector<NYT::NYPath::TYPath> Paths;
};

TString ToString(const TAttributeSelector& selector);

////////////////////////////////////////////////////////////////////////////////

struct TObjectFilter
{
    TString Query;
};

TString ToString(const TObjectFilter& filter);

////////////////////////////////////////////////////////////////////////////////

struct TGetQueryOptions
{
    bool IgnoreNonexistent = false;
    bool FetchValues = true;
    bool FetchTimestamps = false;
};

void FromProto(
    TGetQueryOptions* options,
    const NClient::NApi::NProto::TGetObjectOptions& protoOptions);

////////////////////////////////////////////////////////////////////////////////

struct TGetQueryResult
{
    std::vector<std::optional<TAttributeValueList>> Objects;
};

////////////////////////////////////////////////////////////////////////////////

struct TSelectQueryOptions
{
    std::optional<i64> Offset;
    std::optional<i64> Limit;
};

struct TSelectQueryResult
{
    std::vector<TAttributeValueList> Objects;
};

////////////////////////////////////////////////////////////////////////////////

class TTransaction
    : public NYT::TRefCounted
{
public:
    TTransaction(
        NMaster::TBootstrap* bootstrap,
        TTransactionManagerConfigPtr config,
        const TTransactionId& id,
        TTimestamp startTimestamp,
        NYT::NApi::IClientPtr client,
        NYT::NApi::ITransactionPtr underlyingTransaction);

    ETransactionState GetState() const;

    const TTransactionId& GetId() const;
    TTimestamp GetStartTimestamp() const;

    ISession* GetSession();

    std::unique_ptr<IUpdateContext> CreateUpdateContext();

    TObject* CreateObject(
        EObjectType type,
        const NYT::NYTree::IMapNodePtr& attributes);
    TObject* CreateObject(
        EObjectType type,
        const NYT::NYTree::IMapNodePtr& attributes,
        IUpdateContext* context);

    void RemoveObject(TObject* object);
    void RemoveObject(TObject* object, IUpdateContext* context);

    void UpdateObject(
        TObject* object,
        const std::vector<TUpdateRequest>& requests,
        const std::vector<TAttributeTimestampPrerequisite>& prerequisites = {});
    void UpdateObject(
        TObject* object,
        const std::vector<TUpdateRequest>& requests,
        const std::vector<TAttributeTimestampPrerequisite>& prerequisites,
        IUpdateContext* context);

    TGetQueryResult ExecuteGetQuery(
        EObjectType type,
        const std::vector<TObjectId>& ids,
        const TAttributeSelector& selector,
        const TGetQueryOptions& options);
    TSelectQueryResult ExecuteSelectQuery(
        EObjectType type,
        const std::optional<TObjectFilter>& filter,
        const TAttributeSelector& selector,
        const TSelectQueryOptions& options);

    NYT::NApi::IUnversionedRowsetPtr SelectFields(
        EObjectType objectType,
        const std::vector<const TDBField*>& fields);

    TObject* GetObject(
        EObjectType type,
        const TObjectId& id,
        const TObjectId& parentId = {});

    TSchema* GetSchema(EObjectType type);

    TNode* GetNode(const TObjectId& id);
    TNode* CreateNode(const TObjectId& id = TObjectId());

    TNodeSegment* GetNodeSegment(const TObjectId& id);

    TPod* GetPod(const TObjectId& id);

    TPodSet* GetPodSet(const TObjectId& id);

    TResource* GetResource(const TObjectId& id);

    TNetworkProject* GetNetworkProject(const TObjectId& id);

    TVirtualService* GetVirtualService(const TObjectId& id);

    TDnsRecordSet* GetDnsRecordSet(const TObjectId& id);

    TDnsRecordSet* CreateDnsRecordSet(const TObjectId& id);

    TInternetAddress* GetInternetAddress(const TObjectId& id);

    TAccount* GetAccount(const TObjectId& id);

    TUser* GetUser(const TObjectId& id);

    TGroup* GetGroup(const TObjectId& id);

    TPodDisruptionBudget* GetPodDisruptionBudget(const TObjectId& id);

    TFuture<TTransactionCommitResult> Commit();
    void Abort();

    void ScheduleNotifyAgent(TNode* node);
    void ScheduleAllocateResources(TPod* pod);
    void ScheduleValidateNodeResources(TNode* node);
    void ScheduleUpdatePodSpec(TPod* pod);
    void ScheduleValidateAccounting(TPod* pod);

    NYT::NConcurrency::TAsyncSemaphoreGuard AcquireLock();

private:
    class TImpl;
    using TImplPtr = TIntrusivePtr<TImpl>;
    const TImplPtr Impl_;

};

DEFINE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
