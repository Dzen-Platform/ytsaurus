#include "domestic_medium.h"

#include "config.h"

#include <yt/yt/server/master/cell_master/serialize.h>

namespace NYT::NChunkServer {

using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

bool TDomesticMedium::IsDomestic() const
{
    return true;
}

TString TDomesticMedium::GetLowercaseObjectName() const
{
    return Format("domestic medium %Qv", GetName());
}

TString TDomesticMedium::GetCapitalizedObjectName() const
{
    return Format("Domestic medium %Qv", GetName());
}

void TDomesticMedium::Save(NCellMaster::TSaveContext& context) const
{
    TMediumBase::Save(context);

    using NYT::Save;
    Save(context, Transient_);
    Save(context, *Config_);
    Save(context, DiskFamilyWhitelist_);
}

void TDomesticMedium::Load(NCellMaster::TLoadContext& context)
{
    TMediumBase::Load(context);

    using NYT::Load;

    // COMPAT(gritukan)
    if (context.GetVersion() < EMasterReign::MediumBase) {
        Load(context, Name_);
        Load(context, Index_);
        Load(context, Priority_);
    }

    Load(context, Transient_);

    // COMPAT(gritukan)
    if (context.GetVersion() < EMasterReign::RemoveCacheMedium) {
        Load<bool>(context);
    }

    Load(context, *Config_);

    // COMPAT(gritukan)
    if (context.GetVersion() < EMasterReign::MediumBase) {
        Load(context, Acd_);
    }

    Load(context, DiskFamilyWhitelist_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
