#pragma once

#include <yt/core/misc/guid.h>
#include <yt/core/misc/string.h>
#include <yt/core/misc/small_vector.h>

#include <yt/client/election/public.h>

namespace NYT::NObjectClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((PrerequisiteCheckFailed)                (1000))
    ((InvalidObjectLifeStage)                 (1001))
    ((CrossCellAdditionalPath)                (1002))
    ((CrossCellRevisionPrerequisitePath)      (1003))
);

////////////////////////////////////////////////////////////////////////////////

//! Provides a globally unique identifier for an object.
/*!
 *  TGuid consists of four 32-bit parts.
 *  For TObjectId, these parts have the following meaning:
 *
 *  Part 0: some hash
 *  Part 1: bits 0..15:  object type
 *          bits 16..31: cell id
 *  Part 2: the lo1wer  part of 64-bit sequential counter
 *  Part 3: the higher part of 64-bit sequential counter
 */
using TObjectId = TGuid;

//! The all-zero id used to denote a non-existing object.
constexpr TObjectId NullObjectId = {};

//! Used to mark counters for well-known ids.
constexpr ui64 WellKnownCounterMask = 0x1000000000000000;

using NElection::TCellId;
using NElection::NullCellId;

//! Identifies a particular cell of YT cluster.
//! Must be globally unique to prevent object ids from colliding.
using TCellTag = ui16;

//! The minimum valid cell tag.
constexpr TCellTag MinValidCellTag = 0x0000;

//! The maximum valid cell tag.
constexpr TCellTag MaxValidCellTag = 0xf000;

//! A sentinel cell tag indicating that the request does not need replication.
constexpr TCellTag NotReplicatedCellTag = 0xf001;

//! A sentinel cell tag representing the primary master.
constexpr TCellTag PrimaryMasterCellTag = 0xf003;

//! A sentinel cell tag meaning nothing.
constexpr TCellTag InvalidCellTag = 0xf004;

//! A static limit for the number of secondary master cells.
constexpr int MaxSecondaryMasterCells = 32;

using TCellTagList = SmallVector<TCellTag, MaxSecondaryMasterCells + 1>;

//! Currently at most one additional path is expected (source paths for Copy and Move verbs).
constexpr int TypicalAdditionalPathCount = 1;

//! Describes the runtime type of an object.
DEFINE_ENUM(EObjectType,
    // Does not represent any actual type.
    ((Null)                         (0))

    // The following represent non-versioned objects.
    // These must be created by calling TMasterYPathProxy::CreateObjects.

    // Transaction Manager stuff
    ((Transaction)                (  1))
    ((AtomicTabletTransaction)    (  2))
    ((NonAtomicTabletTransaction) (  3))
    ((NestedTransaction)          (  4))
    ((TransactionMap)             (407))
    ((TopmostTransactionMap)      (418))
    ((LockMap)                    (422))

    // Chunk Manager stuff
    ((Chunk)                      (100))
    ((ErasureChunk)               (102)) // erasure chunk as a whole
    ((ErasureChunkPart_0)         (103)) // erasure chunk parts, mnemonic names are for debugging convenience only
    ((ErasureChunkPart_1)         (104))
    ((ErasureChunkPart_2)         (105))
    ((ErasureChunkPart_3)         (106))
    ((ErasureChunkPart_4)         (107))
    ((ErasureChunkPart_5)         (108))
    ((ErasureChunkPart_6)         (109))
    ((ErasureChunkPart_7)         (110))
    ((ErasureChunkPart_8)         (111))
    ((ErasureChunkPart_9)         (112))
    ((ErasureChunkPart_10)        (113))
    ((ErasureChunkPart_11)        (114))
    ((ErasureChunkPart_12)        (115))
    ((ErasureChunkPart_13)        (116))
    ((ErasureChunkPart_14)        (117))
    ((ErasureChunkPart_15)        (118))
    ((JournalChunk)               (119))
    ((Artifact)                   (121))
    ((ChunkMap)                   (402))
    ((LostChunkMap)               (403))
    ((LostVitalChunkMap)          (413))
    ((PrecariousChunkMap)         (410))
    ((PrecariousVitalChunkMap)    (411))
    ((OverreplicatedChunkMap)     (404))
    ((UnderreplicatedChunkMap)    (405))
    ((DataMissingChunkMap)        (419))
    ((ParityMissingChunkMap)      (420))
    ((QuorumMissingChunkMap)      (424))
    ((UnsafelyPlacedChunkMap)     (120))
    ((ForeignChunkMap)            (122))
    ((ChunkList)                  (101))
    ((ChunkListMap)               (406))
    ((ChunkView)                  (123))
    ((ChunkViewMap)               (430))
    ((Medium)                     (408))
    ((MediumMap)                  (409))

    // The following represent versioned objects (AKA Cypress nodes).
    // These must be created by calling TCypressYPathProxy::Create.
    // NB: When adding a new type, don't forget to update IsVersionedType.

    // Auxiliary
    ((Lock)                       (200))

    // Static nodes
    ((StringNode)                 (300))
    ((Int64Node)                  (301))
    ((Uint64Node)                 (306))
    ((DoubleNode)                 (302))
    ((MapNode)                    (303))
    ((ListNode)                   (304))
    ((BooleanNode)                (305))

    // Dynamic nodes
    ((File)                       (400))
    ((Table)                      (401))
    ((Journal)                    (423))
    ((Orchid)                     (412))
    ((Link)                       (417))
    ((Document)                   (421))
    ((ReplicatedTable)            (425))

    // Portals
    ((PortalEntrance)            (11000))
    ((PortalExit)                (11001))
    ((PortalEntranceMap)         (11002))
    ((PortalExitMap)             (11003))
    ((CypressShard)              (11004))
    ((CypressShardMap)           (11005))

    // Security Manager stuff
    ((Account)                    (500))
    ((AccountMap)                 (414))
    ((User)                       (501))
    ((UserMap)                    (415))
    ((Group)                      (502))
    ((GroupMap)                   (416))

    // Global stuff
    // A mysterious creature representing the master as a whole.
    ((Master)                     (600))
    ((ClusterCell)                (601))
    ((SysNode)                    (602))

    // Tablet Manager stuff
    ((TabletCell)                 (700))
    ((TabletCellNode)             (701))
    ((Tablet)                     (702))
    ((TabletMap)                  (703))
    ((TabletCellMap)              (710))
    ((SortedDynamicTabletStore)   (704))
    ((OrderedDynamicTabletStore)  (708))
    ((TabletPartition)            (705))
    ((TabletCellBundle)           (706))
    ((TabletCellBundleMap)        (707))
    ((TableReplica)               (709))
    ((TabletAction)               (711))
    ((TabletActionMap)            (712))

    // Node Tracker stuff
    ((Rack)                       (800))
    ((RackMap)                    (801))
    ((ClusterNode)                (802))
    ((ClusterNodeNode)            (803))
    ((ClusterNodeMap)             (804))
    ((DataCenter)                 (805))
    ((DataCenterMap)              (806))

    // Job Tracker stuff
    ((SchedulerJob)               (900))
    ((MasterJob)                  (901))

    // Scheduler
    ((Operation)                 (1000))

    // Object manager stuff
    ((EstimatedCreationTimeMap)  (1100))
);

//! A bit mask marking schema types.
constexpr ui32 SchemaObjectTypeMask = 0x8000;

// The range of erasure chunk part types.
constexpr EObjectType MinErasureChunkPartType = EObjectType::ErasureChunkPart_0;
constexpr EObjectType MaxErasureChunkPartType = EObjectType::ErasureChunkPart_15;

////////////////////////////////////////////////////////////////////////////////

using TTransactionId = TObjectId;
constexpr TTransactionId NullTransactionId = {};

using TOperationId = TObjectId;
using TJobId = TObjectId;

////////////////////////////////////////////////////////////////////////////////

//! Identifies a node possibly branched by a transaction.
struct TVersionedObjectId
{
    //! Id of the node itself.
    TObjectId ObjectId;

    //! Id of the transaction that had branched the node.
    //! #NullTransactionId if the node is not branched.
    TTransactionId TransactionId;

    //! Initializes a null instance.
    /*!
     *  #NodeId is #NullObjectId, #TransactionId is #NullTransactionId.
     */
    TVersionedObjectId() = default;

    //! Initializes an instance by given node. Sets #TransactionId to #NullTransactionId.
    explicit TVersionedObjectId(TObjectId objectId);

    //! Initializes an instance by given node and transaction ids.
    TVersionedObjectId(TObjectId objectId, TTransactionId transactionId);

    //! Checks that the id is branched, i.e. #TransactionId is not #NullTransactionId.
    bool IsBranched() const;


    static TVersionedObjectId FromString(TStringBuf str);
};

//! Formats id into a string (for debugging and logging purposes mainly).
void FormatValue(TStringBuilderBase* builder, const TVersionedObjectId& id, TStringBuf spec);

//! Converts id into a string (for debugging and logging purposes mainly).
TString ToString(const TVersionedObjectId& id);

//! Compares TVersionedNodeId s for equality.
bool operator == (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for inequality.
bool operator != (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for "less than".
bool operator <  (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

class TObjectServiceProxy;

struct TDirectObjectIdHash;
struct TDirectVersionedObjectIdHash;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectClient

////////////////////////////////////////////////////////////////////////////////

Y_DECLARE_PODTYPE(NYT::NObjectClient::TVersionedObjectId);
