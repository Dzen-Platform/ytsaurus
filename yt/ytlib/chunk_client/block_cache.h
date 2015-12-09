#pragma once

#include "block_id.h"

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/misc/ref.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! A simple synchronous interface for caching chunk blocks.
/*!
 *  \note
 *  Thread affinity: any
 */
struct IBlockCache
    : public virtual TRefCounted
{
    //! Puts a block into the cache.
    /*!
     *  If a block with the given id is already present, then the request is ignored.
     *
     *  #sourceAddress is an address of peer from which the block was downloaded.
     *  If the block was not downloaded from another peer, it must be Null.
     */
    virtual void Put(
        const TBlockId& id,
        EBlockType type,
        const TSharedRef& data,
        const TNullable<NNodeTrackerClient::TNodeDescriptor>& source) = 0;

    //! Fetches a block from the cache.
    /*!
     *  If no such block is present, then null is returned.
     */
    virtual TSharedRef Find(
        const TBlockId& id,
        EBlockType type) = 0;

    //! Returns the set of supported block types.
    virtual EBlockType GetSupportedBlockTypes() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IBlockCache)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
