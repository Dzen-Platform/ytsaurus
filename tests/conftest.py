from yp.common import YtResponseError
from yp.local import YpInstance, ACTUAL_DB_VERSION
from yp.logger import logger

from yt.wrapper.common import generate_uuid
from yt.wrapper.retries import run_with_retries
from yt.environment.helpers import wait

import pytest

import os
import sys
import logging
import time

# TODO(ignat): avoid this hacks
try:
    import yatest.common as yatest_common
except ImportError:
    yatest_common = None

if yatest_common is not None:
    from yt.environment import arcadia_interop
else:
    arcadia_interop = None

TESTS_LOCATION = os.path.dirname(os.path.abspath(__file__))
TESTS_SANDBOX = os.environ.get("TESTS_SANDBOX", TESTS_LOCATION + ".sandbox")
OBJECT_TYPES = [
    "pod",
    "pod_set",
    "resource",
    "network_project",
    "node",
    "endpoint",
    "endpoint_set",
    "node_segment",
    "user",
    "group",
    "internet_address",
    "account"
]

NODE_CONFIG = {
    "tablet_node": {
        "resource_limits": {
            "tablet_static_memory": 100 * 1024 * 1024,
        }
    }
}

logger.setLevel(logging.DEBUG)

class YpTestEnvironment(object):
    def __init__(self, yp_master_config=None, enable_ssl=False, start=True, db_version=ACTUAL_DB_VERSION):
        if yatest_common is not None:
            destination = os.path.join(yatest_common.work_path(), "build")
            os.makedirs(destination)
            path, node_path = arcadia_interop.prepare_yt_environment(destination)
            os.environ["PATH"] = os.pathsep.join([path, os.environ.get("PATH", "")])
            os.environ["NODE_PATH"] = node_path
            self.test_sandbox_path = os.path.join(yatest_common.output_path(), "yp_" + generate_uuid())
        else:
            self.test_sandbox_path = os.path.join(TESTS_SANDBOX, "yp_" + generate_uuid())

        self.yp_instance = YpInstance(self.test_sandbox_path,
                                      yp_master_config=yp_master_config,
                                      local_yt_options=dict(enable_debug_logging=True, node_config=NODE_CONFIG),
                                      enable_ssl=enable_ssl,
                                      db_version=db_version)
        if start:
            self._start()
        else:
            self.yp_instance.prepare()
        self.yt_client = self.yp_instance.create_yt_client()
        self.sync_access_control()

    def _start(self):
        self.yp_instance.start()
        self.yp_client = self.yp_instance.create_client()

        def touch_pod_set():
            try:
                pod_set_id = self.yp_client.create_object("pod_set")
                self.yp_client.remove_object("pod_set", pod_set_id)
            except YtResponseError:
                return False
            return True

        wait(touch_pod_set)

    def sync_access_control(self):
        # TODO(babenko): improve
        time.sleep(1.0)

    def cleanup(self):
        self.yp_instance.stop()

def test_method_setup(yp_env):
    print >>sys.stderr, "\n"

def test_method_teardown(yp_env):
    print >>sys.stderr, "\n"
    yp_client = yp_env.yp_client
    for object_type in OBJECT_TYPES:
        if object_type == "schema":
            continue

        # Occasionally we may run into conflicts with the scheduler, see YP-284
        def do():
            object_ids = yp_client.select_objects(object_type, selectors=["/meta/id"])
            for object_id_list in object_ids:
                object_id = object_id_list[0]
                if object_type == "user" and object_id == "root":
                    continue
                if object_type == "group" and object_id == "superusers":
                    continue
                if object_type == "account" and object_id == "tmp":
                    continue
                if object_type == "node_segment" and object_id == "default":
                    continue
                yp_client.remove_object(object_type, object_id)
        run_with_retries(do, exceptions=(YtResponseError,))


@pytest.fixture(scope="session")
def test_environment(request):
    environment = YpTestEnvironment()
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="function")
def yp_env(request, test_environment):
    test_method_setup(test_environment)
    request.addfinalizer(lambda: test_method_teardown(test_environment))
    return test_environment

@pytest.fixture(scope="class")
def test_environment_configurable(request):
    environment = YpTestEnvironment(
        yp_master_config=getattr(request.cls, "YP_MASTER_CONFIG", None),
        enable_ssl=getattr(request.cls, "ENABLE_SSL", False))
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="function")
def yp_env_configurable(request, test_environment_configurable):
    test_method_setup(test_environment_configurable)
    request.addfinalizer(lambda: test_method_teardown(test_environment_configurable))
    return test_environment_configurable

@pytest.fixture(scope="function")
def yp_env_migration(request):
    environment = YpTestEnvironment(start=False, db_version=1)
    return environment
