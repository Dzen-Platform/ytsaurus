#pragma once

#include "public.h"

#include <yt/core/misc/error.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

class TCellConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Cell id.
    TCellId CellId;

    //! Peer addresses. Some could be |Null| to indicate that the peer is temporarily missing.
    std::vector<TNullable<Stroka>> Addresses;

    void ValidateAllPeersPresent()
    {
        for (int index = 0; index < Addresses.size(); ++index) {
            if (!Addresses[index]) {
                THROW_ERROR_EXCEPTION("Peer %v is missing in configuration of cell %v",
                    index,
                    CellId);
            }
        }
    }

    TCellConfig()
    {
        RegisterParameter("cell_id", CellId);
        RegisterParameter("addresses", Addresses);

        RegisterValidator([&] () {
           if (!CellId) {
               THROW_ERROR_EXCEPTION("\"cell_id\" cannot be equal to %v",
                   NullCellId);
           }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TCellConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
