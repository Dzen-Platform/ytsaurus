#pragma once

#include "public.h"
#include "master_table_schema.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/chunk_server/public.h>

#include <yt/yt/server/master/transaction_server/transaction.h>

#include <yt/yt/server/lib/hydra/entity_map.h>

#include <yt/yt/core/misc/ref_counted.h>

namespace NYT::NTableServer {

///////////////////////////////////////////////////////////////////////////////

class TTableManager
    : public TRefCounted
{
public:
    explicit TTableManager(NCellMaster::TBootstrap* bootstrap);
    virtual ~TTableManager() override;

    DECLARE_ENTITY_MAP_ACCESSORS(MasterTableSchema, TMasterTableSchema);
    DECLARE_ENTITY_MAP_ACCESSORS(TableCollocation, TTableCollocation);

    void Initialize();

    void ScheduleStatisticsUpdate(
        NChunkServer::TChunkOwnerBase* chunkOwner,
        bool updateDataStatistics = true,
        bool updateTabletStatistics = true,
        bool useNativeContentRevisionCas = false);

    void SendStatisticsUpdate(
        NChunkServer::TChunkOwnerBase* chunkOwner,
        bool useNativeContentRevisionCas = false);

    // COMPAT(shakurov)
    void LoadStatisticsUpdateRequests(NCellMaster::TLoadContext& context);

    //! Looks up a table by id. Throws if no such table exists.
    TTableNode* GetTableNodeOrThrow(TTableId id);

    //! Looks up a master table schema by id. Throws if no such schema exists.
    TMasterTableSchema* GetMasterTableSchemaOrThrow(TMasterTableSchemaId id);

    //! Looks up a table schema. Returns an existing schema object or nullptr if
    //! no such schema exists. This is the means of schema deduplication.
    TMasterTableSchema* FindMasterTableSchema(const NTableClient::TTableSchema& schema) const;

    //! Looks up a schema or creates one if no such schema exists.
    /*!
     *  #schemaHolder will have its schema set to the resulting schema.
     *  The schema itself will be referenced by the table.
     *
     *  NB: This is the means of schema deduplication.
     */
    TMasterTableSchema* GetOrCreateMasterTableSchema(
        const NTableClient::TTableSchema& schema,
        TTableNode* schemaHolder);

    //! Same as above but associates resulting schema with a transaction instead
    //! of a table.
    TMasterTableSchema* GetOrCreateMasterTableSchema(
        const NTableClient::TTableSchema& schema,
        NTransactionServer::TTransaction* schemaHolder);

    //! Creates a new schema object with a specified ID.
    //! The object will be free-floating and will have zero refcounter.
    // COMPAT(shakurov)
    TMasterTableSchema* CreateMasterTableSchemaUnsafely(
        TMasterTableSchemaId schemaId,
        const NTableClient::TTableSchema& schema);

    // For loading from snapshot.
    TMasterTableSchema::TTableSchemaToObjectMapIterator RegisterSchema(
        TMasterTableSchema* schema,
        NTableClient::TTableSchema tableSchema);

    TMasterTableSchema* GetEmptyMasterTableSchema();

    // COMPAT(shakurov)
    TMasterTableSchema* GetOrCreateEmptyMasterTableSchema();

    void SetTableSchema(TTableNode* table, TMasterTableSchema* schema);
    void ResetTableSchema(TTableNode* table);

    //! Table collocation management.
    TTableCollocation* CreateTableCollocation(
        NObjectClient::TObjectId hintId,
        ETableCollocationType type,
        THashSet<TTableNode*> collocatedTables);
    void ZombifyTableCollocation(TTableCollocation* collocation);
    void AddTableToCollocation(TTableNode* table, TTableCollocation* collocation);
    void RemoveTableFromCollocation(TTableNode* table, TTableCollocation* collocation);
    TTableCollocation* GetTableCollocationOrThrow(TTableCollocationId id) const;

private:
    class TImpl;
    class TMasterTableSchemaTypeHandler;

    const TIntrusivePtr<TImpl> Impl_;

    friend class TMasterTableSchema;
};

DEFINE_REFCOUNTED_TYPE(TTableManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableServer
