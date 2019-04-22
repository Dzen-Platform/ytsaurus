import urllib2
import time
import json

import pytest

import yt.packages.requests as requests

from yt_env_setup import YTEnvSetup, wait
from yt_commands import *

##################################################################

class TestMonitoring(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    ENABLE_PROXY = True
    DELTA_PROXY_CONFIG = {
        "api": {
            "force_tracing": True,
        },
    }

    def get_json(self, port, query):
        return json.loads(urllib2.urlopen("http://localhost:{}/orchid{}".format(port, query)).read())
    
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
                events = self.get_json(http_port, "/profiling/logging/backlog_events?from_time={}".format(int(start_time) * 1000000))
                return len(events) > 0
            except urllib2.HTTPError:
                return False

        wait(monitoring_orchid_ready)

        with pytest.raises(urllib2.HTTPError):
            self.get_json(http_port, "/profiling/logging/backlog_events?from_time=abc")

    @pytest.mark.parametrize("component", ["master"])
    def test_component_http_tracing(self, component):
        http_port = self.Env.configs[component][0]["monitoring_port"]
        url = "http://localhost:{}/tracing/traces?start_index=0&limit=1000".format(http_port)
        
        rsp = requests.get(url)
        assert rsp.status_code == 200
        assert "X-YT-Process-Id" in rsp.headers
        assert "X-YT-Trace-Start-Index" in rsp.headers
        assert len(rsp.content) > 0

        process_id = rsp.headers["X-YT-Process-Id"]

        rsp = requests.get(url, headers={"X-YT-Check-Process-Id": process_id})
        assert rsp.status_code == 200

        rsp = requests.get(url, headers={"X-YT-Check-Process-Id": process_id + "-foo"})
        assert rsp.status_code == 412

