#include "private.h"

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

const TString IntermediatePath = "<intermediate>";

const NLogging::TLogger ControllerLogger("Controller");
const NLogging::TLogger ControllerAgentLogger("ControllerAgent");

const NProfiling::TProfiler ControllerAgentProfiler("/controller_agent");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
