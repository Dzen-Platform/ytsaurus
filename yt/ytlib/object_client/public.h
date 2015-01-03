#pragma once

#include <core/misc/guid.h>
#include <core/misc/string.h>

namespace NYT {
namespace NObjectClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((PrerequisiteCheckFailed)     (1000))
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
 *  Part 2: the lower  part of 64-bit sequential counter
 *  Part 3: the higher part of 64-bit sequential counter
 */
typedef TGuid TObjectId;

//! The all-zero id used to denote a non-existing object.
extern TObjectId NullObjectId;

//! Used to mark counters for well-known ids.
const ui64 WellKnownCounterMask = 0x1000000000000000;

//! Identifies a particular installation of YT.
//! Must be unique to prevent object ids from colliding.
typedef ui16 TCellTag;

//! Describes the runtime type of an object.
DEFINE_ENUM(EObjectType,
    // Does not represent any actual type.
    ((Null)                         (0))

    // The following represent non-versioned objects.
    // These must be created by calling TMasterYPathProxy::CreateObjects.

    // Transaction Manager stuff
    ((Transaction)                (  1))
    ((TransactionMap)             (407))
    ((TopmostTransactionMap)      (418))
    ((TabletTransaction)          (  2))
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
    ((ChunkMap)                   (402))
    ((LostChunkMap)               (403))
    ((LostVitalChunkMap)          (413))
    ((OverreplicatedChunkMap)     (404))
    ((UnderreplicatedChunkMap)    (405))
    ((DataMissingChunkMap)        (419))
    ((ParityMissingChunkMap)      (420))
    ((QuorumMissingChunkMap)      (424))
    ((UnsafelyPlacedChunkMap)     (120))
    ((ChunkList)                  (101))
    ((ChunkListMap)               (406))
        
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

    // Tablet Manager stuff
    ((TabletCell)                 (700))
    ((TabletCellNode)             (701))
    ((Tablet)                     (702))
    ((TabletMap)                  (703))
    ((DynamicMemoryTabletStore)   (704))
    ((TabletPartition)            (705))
    
    // Node Tracker stuff
    ((CellNodeMap)                (408))
    ((CellNode)                   (410))
    ((Rack)                       (800))
    ((RackMap)                    (801))
);

//! A bit mask marking schema types.
const int SchemaObjectTypeMask = 0x8000;

// The range of valid object types (including schemas).
const EObjectType MinObjectType = TEnumTraits<EObjectType>::GetMinValue();
const EObjectType MaxObjectType = EObjectType(
    static_cast<int>(TEnumTraits<EObjectType>::GetMaxValue()) +
    SchemaObjectTypeMask);

////////////////////////////////////////////////////////////////////////////////

typedef TObjectId TTransactionId;
extern TTransactionId NullTransactionId;

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
     *  #NodeId id #NullObjectId, #TransactionId is #NullTransactionId.
     */
    TVersionedObjectId();

    //! Initializes an instance by given node. Sets #TransactionId to #NullTransactionId.
    explicit TVersionedObjectId(const TObjectId& objectId);

    //! Initializes an instance by given node and transaction ids.
    TVersionedObjectId(const TObjectId& objectId, const TTransactionId& transactionId);

    //! Checks that the id is branched, i.e. #TransactionId is not #NullTransactionId.
    bool IsBranched() const;


    static TVersionedObjectId FromString(const TStringBuf& str);
};

//! Formats id into a string (for debugging and logging purposes mainly).
void FormatValue(TStringBuilder* builder, const TVersionedObjectId& id);

//! Converts id into a string (for debugging and logging purposes mainly).
Stroka ToString(const TVersionedObjectId& id);

//! Compares TVersionedNodeId s for equality.
bool operator == (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for inequality.
bool operator != (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

//! Compares TVersionedNodeId s for "less than" (used to sort nodes in meta-map).
bool operator <  (const TVersionedObjectId& lhs, const TVersionedObjectId& rhs);

} // namespace NObjectClient
} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

DECLARE_PODTYPE(NYT::NObjectClient::TVersionedObjectId);

//! A hasher for TVersionedNodeId.
template <>
struct hash<NYT::NObjectClient::TVersionedObjectId>
{
    size_t operator() (const NYT::NObjectClient::TVersionedObjectId& id) const
    {
        return THash<NYT::TGuid>()(id.TransactionId) * 497 +
               THash<NYT::TGuid>()(id.ObjectId);
    }
};

////////////////////////////////////////////////////////////////////////////////

