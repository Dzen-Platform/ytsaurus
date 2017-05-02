#pragma once

#include "public.h"
#include "schema.h"
#include "type_handler.h"

#include <yt/server/cell_master/automaton.h>

#include <yt/server/cypress_server/public.h>

#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/object_manager.pb.h>

#include <yt/server/security_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/ytlib/object_client/object_ypath_proxy.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NObjectServer {

// WinAPI is great.
#undef GetObject

////////////////////////////////////////////////////////////////////////////////

//! Similar to NYTree::INodeResolver but works for arbitrary objects rather than nodes.
struct IObjectResolver
{
    virtual ~IObjectResolver() = default;

    //! Resolves a given path in the context of a given transaction.
    //! Throws if resolution fails.
    virtual IObjectProxyPtr ResolvePath(
        const NYPath::TYPath& path,
        NTransactionServer::TTransaction* transaction) = 0;

    //! Returns a path corresponding to a given object.
    virtual NYPath::TYPath GetPath(IObjectProxyPtr proxy) = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! Provides high-level management and tracking of objects.
/*!
 *  \note
 *  Thread affinity: single-threaded
 */
class TObjectManager
    : public NCellMaster::TMasterAutomatonPart
{
public:
    TObjectManager(
        TObjectManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    void Initialize();

    //! Registers a new type handler.
    void RegisterHandler(IObjectTypeHandlerPtr handler);

    //! Returns the handler for a given type or |nullptr| if the type is unknown.
    const IObjectTypeHandlerPtr& FindHandler(EObjectType type) const;

    //! Returns the handler for a given type.
    const IObjectTypeHandlerPtr& GetHandler(EObjectType type) const;

    //! Returns the handler for a given object.
    const IObjectTypeHandlerPtr& GetHandler(const TObjectBase* object) const;

    //! Returns the set of registered object types, excluding schemas.
    const std::set<EObjectType>& GetRegisteredTypes() const;

    //! If |hintId| is |NullObjectId| then creates a new unique object id.
    //! Otherwise returns |hintId| (but checks its type).
    TObjectId GenerateId(EObjectType type, const TObjectId& hintId);

    //! Adds a reference.
    //! Returns the strong reference counter.
    int RefObject(TObjectBase* object);

    //! Removes #count references.
    //! Returns the strong reference counter.
    int UnrefObject(TObjectBase* object, int count = 1);

    //! Returns the current strong reference counter.
    int GetObjectRefCounter(TObjectBase* object);

    //! Increments the object weak reference counter thus temporarily preventing it from being destructed.
    //! Returns the weak reference counter.
    int WeakRefObject(TObjectBase* object);

    //! Decrements the object weak reference counter thus making it eligible for destruction.
    //! Returns the weak reference counter.
    int WeakUnrefObject(TObjectBase* object);

    //! Returns the current weak reference counter.
    int GetObjectWeakRefCounter(TObjectBase* object);

    //! Finds object by id, returns |nullptr| if nothing is found.
    TObjectBase* FindObject(const TObjectId& id);

    //! Finds object by id, fails if nothing is found.
    TObjectBase* GetObject(const TObjectId& id);

    //! Finds object by id, throws if nothing is found.
    TObjectBase* GetObjectOrThrow(const TObjectId& id);

    //! Creates a cross-cell read-only proxy for the object with the given #id.
    NYTree::IYPathServicePtr CreateRemoteProxy(const TObjectId& id);

    //! Returns a proxy for the object with the given versioned id.
    IObjectProxyPtr GetProxy(
        TObjectBase* object,
        NTransactionServer::TTransaction* transaction = nullptr);

    //! Called when a versioned object is branched.
    void BranchAttributes(
        const TObjectBase* originatingObject,
        TObjectBase* branchedObject);

    //! Called when a versioned object is merged during transaction commit.
    void MergeAttributes(
        TObjectBase* originatingObject,
        const TObjectBase* branchedObject);

    //! Fills the attributes of a given unversioned object.
    void FillAttributes(
        TObjectBase* object,
        const NYTree::IAttributeDictionary& attributes);

    //! Returns a YPath service that routes all incoming requests.
    NYTree::IYPathServicePtr GetRootService();

    //! Returns "master" object for handling requests sent via TMasterYPathProxy.
    TObjectBase* GetMasterObject();

    //! Returns a proxy for master object.
    /*!
     *  \see GetMasterObject
     */
    IObjectProxyPtr GetMasterProxy();

    //! Finds a schema object for a given type, returns |nullptr| if nothing is found.
    TObjectBase* FindSchema(EObjectType type);

    //! Finds a schema object for a given type, fails if nothing is found.
    TObjectBase* GetSchema(EObjectType type);

    //! Returns a proxy for schema object.
    /*!
     *  \see GetSchema
     */
    IObjectProxyPtr GetSchemaProxy(EObjectType type);

    //! Creates a mutation that executes a request represented by #context.
    /*!
     *  Thread affinity: any
     */
    NHydra::TMutationPtr CreateExecuteMutation(
        const Stroka& userName,
        const NRpc::IServiceContextPtr& context);

    //! Creates a mutation that destroys given objects.
    /*!
     *  Thread affinity: any
     */
    NHydra::TMutationPtr CreateDestroyObjectsMutation(
        const NProto::TReqDestroyObjects& request);

    //! Returns a future that gets set when the GC queues becomes empty.
    TFuture<void> GCCollect();

    TObjectBase* CreateObject(
        const TObjectId& hintId,
        EObjectType type,
        NYTree::IAttributeDictionary* attributes);

    IObjectResolver* GetObjectResolver();

    //! Validates prerequisites, throws on failure.
    void ValidatePrerequisites(const NObjectClient::NProto::TPrerequisitesExt& prerequisites);

    //! Forwards a request to the leader of a given cell.
    TFuture<TSharedRefArray> ForwardToLeader(
        TCellTag cellTag,
        TSharedRefArray requestMessage,
        TNullable<TDuration> timeout = Null);

    //! Posts a creation request to the secondary master.
    void ReplicateObjectCreationToSecondaryMaster(
        TObjectBase* object,
        TCellTag cellTag);

    //! Posts a creation request to secondary masters.
    void ReplicateObjectCreationToSecondaryMasters(
        TObjectBase* object,
        const TCellTagList& cellTags);

    //! Posts an attribute update request to the secondary master.
    void ReplicateObjectAttributesToSecondaryMaster(
        TObjectBase* object,
        TCellTag cellTag);

    const NProfiling::TProfiler& GetProfiler();
    NProfiling::TTagId GetTypeTagId(EObjectType type);
    NProfiling::TTagId GetMethodTagId(const Stroka& method);

    TEpoch GetCurrentEpoch();

private:
    friend class TObjectProxyBase;

    class TRootService;
    typedef TIntrusivePtr<TRootService> TRootServicePtr;
    class TRemoteProxy;
    class TObjectResolver;

    const TObjectManagerConfigPtr Config_;

    NProfiling::TProfiler Profiler;

    struct TTypeEntry
    {
        IObjectTypeHandlerPtr Handler;
        TSchemaObject* SchemaObject = nullptr;
        IObjectProxyPtr SchemaProxy;
        NProfiling::TTagId TagId;
    };

    std::set<EObjectType> RegisteredTypes_;
    TEnumIndexedVector<TTypeEntry, EObjectType, MinObjectType, MaxObjectType> TypeToEntry_;

    yhash<Stroka, NProfiling::TTagId> MethodToTag_;

    TRootServicePtr RootService_;

    std::unique_ptr<TObjectResolver> ObjectResolver_;

    TObjectId MasterObjectId_;
    std::unique_ptr<TMasterObject> MasterObject_;

    IObjectProxyPtr MasterProxy_;

    NConcurrency::TPeriodicExecutorPtr ProfilingExecutor_;

    TGarbageCollectorPtr GarbageCollector_;

    int CreatedObjects_ = 0;
    int DestroyedObjects_ = 0;

    //! Stores schemas (for serialization mostly).
    NHydra::TEntityMap<TSchemaObject> SchemaMap_;

    TEpoch CurrentEpoch_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(NCellMaster::TSaveContext& context) const;
    void SaveValues(NCellMaster::TSaveContext& context) const;

    void LoadKeys(NCellMaster::TLoadContext& context);
    void LoadValues(NCellMaster::TLoadContext& context);

    virtual void OnRecoveryStarted() override;
    virtual void OnRecoveryComplete() override;
    virtual void Clear() override;
    virtual void OnLeaderActive() override;
    virtual void OnStopLeading() override;

    void HydraExecuteLeader(
        const Stroka& userName,
        const NRpc::IServiceContextPtr& context,
        NHydra::TMutationContext*);
    void HydraExecuteFollower(NProto::TReqExecute* request);
    void HydraDestroyObjects(NProto::TReqDestroyObjects* request);
    void HydraCreateForeignObject(NProto::TReqCreateForeignObject* request) noexcept;
    void HydraRemoveForeignObject(NProto::TReqRemoveForeignObject* request) noexcept;
    void HydraUnrefExportedObjects(NProto::TReqUnrefExportedObjects* request) noexcept;

    void OnProfiling();

    std::unique_ptr<NYTree::IAttributeDictionary> GetReplicatedAttributes(
        TObjectBase* object,
        bool mandatory);
    void OnReplicateValuesToSecondaryMaster(TCellTag cellTag);

    void InitSchemas();

};

DEFINE_REFCOUNTED_TYPE(TObjectManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

