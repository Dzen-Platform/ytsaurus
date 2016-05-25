from yt_env_setup import YTEnvSetup
from yt_commands import *


##################################################################

class TestSnapshot(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 0

    def test(self):
        set("//tmp/a", 42)

        build_snapshot(cell_id=None)

        self.Env.kill_service("master")
        self.Env.start_masters("master")

        assert get("//tmp/a") == 42
