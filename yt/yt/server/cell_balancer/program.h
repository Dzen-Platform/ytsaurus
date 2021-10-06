#pragma once

#include "config.h"

#include <yt/yt/ytlib/program/program.h>
#include <yt/yt/ytlib/program/program_config_mixin.h>
#include <yt/yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/yt/ytlib/program/program_setsid_mixin.h>

namespace NYT::NCellBalancer {

////////////////////////////////////////////////////////////////////////////////

class TCellBalancerProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramSetsidMixin
    , public TProgramConfigMixin<TCellBalancerConfig>
{
public:
    TCellBalancerProgram();

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellBalancer
