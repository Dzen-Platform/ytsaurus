#include <yt/yt/library/auth_server/tvm_service.h>

#include <yt/yt/core/test_framework/framework.h>

namespace NYT::NAuth {

class TMockTvmService
    : public ITvmService
{
public:
    MOCK_METHOD(ui32, GetSelfTvmId, (), (override));
    MOCK_METHOD(TString, GetServiceTicket, (const TString&), (override));
    MOCK_METHOD(TString, GetServiceTicket, (ui32), (override));
    MOCK_METHOD(TParsedTicket, ParseUserTicket, (const TString&), (override));
    MOCK_METHOD(TParsedServiceTicket, ParseServiceTicket, (const TString&), (override));
};

} // namespace NYT::NAuth
