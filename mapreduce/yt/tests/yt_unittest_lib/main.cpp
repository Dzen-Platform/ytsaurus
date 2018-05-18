#include <library/unittest/utmain.h>

#include <mapreduce/yt/interface/client.h>
#include <mapreduce/yt/common/config.h>

int main(int argc, const char** argv)
{
    NYT::TConfig::Get()->LogLevel = "debug";
    NYT::Initialize(argc, argv, NYT::TInitializeOptions().CleanupOnTermination(true));
    return NUnitTest::RunMain(argc, const_cast<char**>(argv));
}
