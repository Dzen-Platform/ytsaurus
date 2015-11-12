#include "automaton.h"
#include "bootstrap.h"
#include "hydra_facade.h"
#include "serialize.h"

#include <yt/core/misc/common.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

TMasterAutomaton::TMasterAutomaton(TBootstrap* bootstrap)
    : TCompositeAutomaton(nullptr)
    , SaveContext_(new TSaveContext())
    , LoadContext_(new TLoadContext(bootstrap))
{ }

TSaveContext& TMasterAutomaton::SaveContext()
{
    return *SaveContext_;
}

TLoadContext& TMasterAutomaton::LoadContext()
{
    return *LoadContext_;
}

////////////////////////////////////////////////////////////////////////////////

TMasterAutomatonPart::TMasterAutomatonPart(TBootstrap* bootstrap)
    : TCompositeAutomatonPart(
        bootstrap->GetHydraFacade()->GetHydraManager(),
        bootstrap->GetHydraFacade()->GetAutomaton(),
        bootstrap->GetHydraFacade()->GetAutomatonInvoker())
    , Bootstrap_(bootstrap)
{ }

bool TMasterAutomatonPart::ValidateSnapshotVersion(int version)
{
    return NCellMaster::ValidateSnapshotVersion(version);
}

int TMasterAutomatonPart::GetCurrentSnapshotVersion()
{
    return NCellMaster::GetCurrentSnapshotVersion();
}

void TMasterAutomatonPart::RegisterSaver(
    NHydra::ESyncSerializationPriority priority,
    const Stroka& name,
    TCallback<void(TSaveContext&)> saver)
{
    TCompositeAutomatonPart::RegisterSaver(
        priority,
        name,
        BIND([=] () {
            auto& context = Bootstrap_->GetHydraFacade()->GetAutomaton()->SaveContext();
            saver.Run(context);
        }));
}

void TMasterAutomatonPart::RegisterLoader(
    const Stroka& name,
    TCallback<void(TLoadContext&)> loader)
{
    TCompositeAutomatonPart::RegisterLoader(
        name,
        BIND([=] () {
            auto& context = Bootstrap_->GetHydraFacade()->GetAutomaton()->LoadContext();
            loader.Run(context);
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT

