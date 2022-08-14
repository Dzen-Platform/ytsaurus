#include "tablet_cell_service.h"

#include "bootstrap.h"
#include "private.h"

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/server/node/cellar_node/master_connector.h>

#include <yt/yt/server/node/cluster_node/master_connector.h>

#include <yt/yt/server/node/tablet_node/master_connector.h>

#include <yt/yt/server/lib/hydra_common/hydra_service.h>

#include <yt/yt/ytlib/tablet_cell_client/tablet_cell_service_proxy.h>

namespace NYT::NTabletNode {

using namespace NClusterNode;
using namespace NObjectClient;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellService
    : public TServiceBase
{
public:
    TTabletCellService(IBootstrap* bootstrap, bool fixSpelling)
        : TServiceBase(
            bootstrap->GetControlInvoker(),
            fixSpelling
            ? NTabletCellClient::TTabletCellServiceProxyFixedSpelling::GetDescriptor()
            : NTabletCellClient::TTabletCellServiceProxy::GetDescriptor(),
            TabletNodeLogger)
        , Bootstrap_(bootstrap)
    {
        YT_VERIFY(Bootstrap_);

        RegisterMethod(RPC_SERVICE_METHOD_DESC(RequestHeartbeat));
    }

private:
    IBootstrap* const Bootstrap_;

    DECLARE_RPC_SERVICE_METHOD(NTabletCellClient::NProto, RequestHeartbeat)
    {
        context->SetRequestInfo();

        if (Bootstrap_->IsConnected()) {
            auto primaryCellTag = CellTagFromId(Bootstrap_->GetCellId());
            const auto& masterConnector = Bootstrap_->GetCellarNodeMasterConnector();
            masterConnector->ScheduleHeartbeat(primaryCellTag, /*immediately*/ true);
        }

        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

IServicePtr CreateTabletCellService(IBootstrap* bootstrap, bool fixSpelling)
{
    return New<TTabletCellService>(bootstrap, fixSpelling);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
