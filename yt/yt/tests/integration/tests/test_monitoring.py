import urllib2
import time
import json

import pytest

from yt_env_setup import YTEnvSetup, wait
from yt_commands import *


##################################################################


class TestMonitoring(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    ENABLE_HTTP_PROXY = True
    DELTA_PROXY_CONFIG = {
        "api": {
            "force_tracing": True,
        },
    }

    def get_json(self, port, query):
        return json.loads(urllib2.urlopen("http://localhost:{}/orchid{}".format(port, query)).read())

    @authors("prime")
    @pytest.mark.parametrize("component", ["master", "scheduler", "node"])
    def test_component_http_monitoring(self, component):
        http_port = self.Env.configs[component][0]["monitoring_port"]

        root_orchid = self.get_json(http_port, "")
        assert "config" in root_orchid

        config_orchid = self.get_json(http_port, "/config")
        assert "monitoring_port" in config_orchid

        start_time = time.time()

        def monitoring_orchid_ready():
            try:
                events = self.get_json(
                    http_port,
                    "/profiling/logging/backlog_events?from_time={}".format(int(start_time) * 1000000),
                )
                return len(events) > 0
            except urllib2.HTTPError:
                return False

        wait(monitoring_orchid_ready)

        with pytest.raises(urllib2.HTTPError):
            self.get_json(http_port, "/profiling/logging/backlog_events?from_time=abc")
