#pragma once

#include <yt/yt/core/misc/public.h>

namespace NYT::NSequoiaServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(ISequoiaContext)
DECLARE_REFCOUNTED_STRUCT(ISequoiaManager)

DECLARE_REFCOUNTED_CLASS(TDynamicSequoiaManagerConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EAevum,
    ((None)                                           (0))
    ((SequoiaChunkMetaExtensions)                     (1)) // gritukan
);

EAevum GetCurrentAevum();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSequoiaServer
