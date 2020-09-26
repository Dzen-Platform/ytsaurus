from __future__ import print_function

from .helpers import (get_tests_location, TEST_DIR, get_tests_sandbox,
                      sync_create_cell, get_test_file_path, get_port_locks_path,
                      yatest_common, create_job_events, wait)

from yt.environment import YTInstance, arcadia_interop
from yt.environment.helpers import emergency_exit_within_tests
from yt.wrapper.config import set_option
from yt.wrapper.default_config import get_default_config
from yt.wrapper.common import update, update_inplace, GB
from yt.common import which, makedirp, format_error
import yt.environment.init_operation_archive as init_operation_archive
import yt.subprocess_wrapper as subprocess
from yt.test_helpers.authors import pytest_configure, pytest_collection_modifyitems, pytest_itemcollected  # noqa

from yt.packages.six import iteritems, itervalues

from yt.packages import requests

import yt.wrapper as yt

import pytest

import os
import imp
import sys
import uuid
from copy import deepcopy
import shutil
import logging
import socket
import warnings

# Disables """cryptography/hazmat/primitives/constant_time.py:26: CryptographyDeprecationWarning: Support for your Python version is deprecated.
# The next version of cryptography will remove support. Please upgrade to a 2.7.x release that supports hmac.compare_digest as soon as possible."""
warnings.filterwarnings(action="ignore", module="cryptography.hazmat.primitives.*")

yt.http_helpers.RECEIVE_TOKEN_FROM_SSH_SESSION = False

def authors(*the_authors):
    return pytest.mark.authors(the_authors)

def pytest_ignore_collect(path, config):
    path = str(path)
    return path.startswith(get_tests_sandbox()) or \
        path.startswith(os.path.join(get_tests_location(), "__pycache__"))

def rmtree(path):
    if os.path.exists(path):
        shutil.rmtree(path)

class YtTestEnvironment(object):
    def __init__(self,
                 test_name,
                 config=None,
                 env_options=None,
                 delta_scheduler_config=None,
                 delta_controller_agent_config=None,
                 delta_node_config=None,
                 delta_proxy_config=None,
                 need_suid=False):
        # To use correct version of bindings we must reset it before start environment.
        yt.native_driver.driver_bindings = None

        self.test_name = test_name

        if config is None:
            config = {}
        if env_options is None:
            env_options = {}

        has_http_proxy = config["backend"] not in ("native",)

        logging.getLogger("Yt.local").setLevel(logging.INFO)

        run_id = uuid.uuid4().hex[:8]
        self.uniq_dir_name = os.path.join(self.test_name, "run_" + run_id)
        self.sandbox_dir = os.path.join(get_tests_sandbox(), self.uniq_dir_name)
        self.core_path = os.path.join(get_tests_sandbox(), "_cores")
        if not os.path.exists(self.core_path):
            os.makedirs(self.core_path)

        self.binaries_path = None
        if yatest_common is not None:
            if need_suid and arcadia_interop.is_inside_distbuild():
                pytest.skip()

            ytrecipe = os.environ.get("YTRECIPE") is not None

            suffix = "need_suid_" + str(int(need_suid))
            if yatest_common.get_param("ram_drive_path") is not None:
                prepare_dir = os.path.join(yatest_common.ram_drive_path(), suffix)
            else:
                prepare_dir = os.path.join(yatest_common.work_path(), suffix)

            if not os.path.exists(prepare_dir):
                os.makedirs(prepare_dir)

                self.binaries_path = arcadia_interop.prepare_yt_environment(
                    prepare_dir,
                    copy_ytserver_all=not ytrecipe,
                    need_suid=need_suid and not ytrecipe)
                os.environ["PATH"] = os.pathsep.join([self.binaries_path, os.environ.get("PATH", "")])

        common_delta_node_config = {
            "exec_agent": {
                "slot_manager": {
                    "enforce_job_control": True,
                },
                "statistics_reporter": {
                    "reporting_period": 1000,
                }
            },
        }
        common_delta_scheduler_config = {
            "scheduler": {
                "max_operation_count": 5,
            }
        }

        common_delta_controller_agent_config = {
            "controller_agent": {
                "operation_options": {
                    "spec_template": {
                        "max_failed_job_count": 1
                    }
                }
            },
            "core_dumper": {
                "path": self.core_path,
                # Pattern starts with the underscore to trick teamcity; we do not want it to
                # pay attention to the created core.
                "pattern": "_core.%CORE_PID.%CORE_SIG.%CORE_THREAD_NAME-%CORE_REASON",
            }
        }

        def modify_configs(configs, abi_version):
            for config in configs["scheduler"]:
                update_inplace(config, common_delta_scheduler_config)
                if delta_scheduler_config:
                    update_inplace(config, delta_scheduler_config)
                if configs.get("controller_agent") is None:
                    update_inplace(config["scheduler"], common_delta_controller_agent_config["controller_agent"])

            if configs.get("controller_agent") is not None:
                for config in configs["controller_agent"]:
                    update_inplace(config, common_delta_controller_agent_config)
                    if delta_controller_agent_config:
                        update_inplace(config, delta_controller_agent_config)

            for config in configs["node"]:
                update_inplace(config, common_delta_node_config)
                if delta_node_config:
                    update_inplace(config, delta_node_config)
            for config in configs["http_proxy"]:
                if delta_proxy_config:
                    update_inplace(config, delta_proxy_config)

        local_temp_directory = os.path.join(get_tests_sandbox(), "tmp_" + run_id)
        if not os.path.exists(local_temp_directory):
            os.mkdir(local_temp_directory)

        self.env = YTInstance(self.sandbox_dir,
                              master_count=1,
                              node_count=3,
                              scheduler_count=1,
                              http_proxy_count=1 if has_http_proxy else 0,
                              rpc_proxy_count=1,
                              port_locks_path=get_port_locks_path(),
                              fqdn="localhost",
                              modify_configs_func=modify_configs,
                              kill_child_processes=True,
                              allow_chunk_storage_in_tmpfs=True,
                              **env_options)

        try:
            self.env.start(start_secondary_master_cells=True)
        except:
            self.save_sandbox()
            raise

        self.version = "{0}.{1}".format(*self.env.abi_version)

        # TODO(ignat): Remove after max_replication_factor will be implemented.
        set_option("_is_testing_mode", True, client=None)

        self.config = update(get_default_config(), config)
        self.config["enable_request_logging"] = True
        self.config["enable_passing_request_id_to_driver"] = True
        self.config["operation_tracker"]["poll_period"] = 100
        if has_http_proxy:
            self.config["proxy"]["url"] = "localhost:" + self.env.get_proxy_address().split(":", 1)[1]

        # NB: to decrease probability of retries test failure.
        self.config["proxy"]["retries"]["count"] = 10
        self.config["write_retries"]["count"] = 10

        self.config["proxy"]["retries"]["backoff"]["constant_time"] = 500
        self.config["proxy"]["retries"]["backoff"]["policy"] = "constant_time"

        self.config["read_retries"]["backoff"]["constant_time"] = 500
        self.config["read_retries"]["backoff"]["policy"] = "constant_time"

        self.config["write_retries"]["backoff"]["constant_time"] = 500
        self.config["write_retries"]["backoff"]["policy"] = "constant_time"

        self.config["batch_requests_retries"]["backoff"]["constant_time"] = 500
        self.config["batch_requests_retries"]["backoff"]["policy"] = "constant_time"

        self.config["read_parallel"]["data_size_per_thread"] = 1
        self.config["read_parallel"]["max_thread_count"] = 10

        self.config["enable_token"] = False
        self.config["pickling"]["module_filter"] = lambda module: hasattr(module, "__file__") and not "driver_lib" in module.__file__

        self.config["pickling"]["python_binary"] = sys.executable
        self.config["user_job_spec_defaults"] = {
            "environment": dict([(key, value) for key, value in iteritems(os.environ) if "PYTHON" in key])
        }

        if config["backend"] != "rpc":
            self.config["driver_config"] = self.env.configs["driver"]
        self.config["local_temp_directory"] = local_temp_directory
        self.config["enable_logging_for_params_changes"] = True
        self.config["allow_fallback_to_native_driver"] = False

        # Interrupt main in tests is unrelaible and can cause 'Test crashed' or other errors in case of flaps.
        self.config["ping_failed_mode"] = "pass"

        # NB: temporary hack
        if arcadia_interop.yatest_common is not None:
            self.config["is_local_mode"] = True
        else:
            self.config["is_local_mode"] = False

        self.reload_global_configuration()

        os.environ["PATH"] = ".:" + os.environ["PATH"]

        # To avoid using user-defined proxy in tests.
        if "YT_PROXY" in os.environ:
            del os.environ["YT_PROXY"]

        os.environ["YT_LOCAL_PORT_LOCKS_PATH"] = get_port_locks_path()

        # NB: temporary hack
        if arcadia_interop.yatest_common is not None:
            self.env._create_cluster_client().set("//sys/@local_mode_fqdn", socket.getfqdn())

        self.env._create_cluster_client().set("//sys/@cluster_connection", self.config["driver_config"])

        # Resolve indeterminacy in sys.modules due to presence of lazy imported modules.
        for module in list(itervalues(sys.modules)):
            hasattr(module, "__file__")

    def cleanup(self):
        self.reload_global_configuration()
        self.env.stop()
        self.env.remove_runtime_data()
        self.save_sandbox()

    def save_sandbox(self):
        try:
            arcadia_interop.save_sandbox(self.sandbox_dir, self.uniq_dir_name)
        except:
            # Additional logging added due to https://github.com/pytest-dev/pytest/issues/2237
            logging.exception("YtTestEnvironment cleanup failed")
            raise

    def check_liveness(self):
        self.env.check_liveness(callback_func=emergency_exit_within_tests)

    def reload_global_configuration(self):
        yt.config._init_state()
        yt.native_driver.driver_bindings = None
        yt._cleanup_http_session()
        yt.config.config = self.config

def init_environment_for_test_session(mode, **kwargs):
    config = {"api_version": "v3"}
    if mode in ("native_v3", "native_v4"):
        config["backend"] = "native"
        if mode == "native_v4":
            config["api_version"] = "v4"
    elif mode == "rpc":
        config["backend"] = "rpc"
    elif mode in ("native_multicell", "yamr", "job_archive"):
        config["backend"] = "http"
        config["api_version"] = "v4"
    else:
        config["backend"] = "http"
        config["api_version"] = mode

    environment = YtTestEnvironment(
        "TestYtWrapper" + mode.capitalize(),
        config,
        **kwargs)

    if mode.startswith("native"):
        import yt_driver_bindings
        yt_driver_bindings.configure_logging(environment.env.configs["driver_logging"])
    else:
        yt.config.COMMANDS = None

    return environment

@pytest.fixture(scope="class", params=["v3", "v4", "native_v3", "native_v4"])
def test_environment(request):
    environment = init_environment_for_test_session(request.param)
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class", params=["v3", "v4"])
def test_environment_with_framing(request):
    suspending_path = "//tmp/suspending_table"
    delay_before_command = 10 * 1000
    keep_alive_period = 1 * 1000
    delta_proxy_config = {
        "api": {
            "testing": {
                "delay_before_command": {
                    "read_table": {
                        "parameter_path": "/path",
                        "substring": suspending_path,
                        "delay": delay_before_command,
                    },
                    "get_table_columnar_statistics": {
                        "parameter_path": "/paths/0",
                        "substring": suspending_path,
                        "delay": delay_before_command,
                    },
                },
            },
        },
    }
    environment = init_environment_for_test_session(request.param, delta_proxy_config=delta_proxy_config)

    # Setup framing keep-alive period through dynamic config.
    yt.set("//sys/proxies/@config", {"framing": {"keep_alive_period": keep_alive_period}})
    monitoring_port = environment.env.configs["http_proxy"][0]["monitoring_port"]
    config_url = "http://localhost:{}/orchid/coordinator/dynamic_config".format(monitoring_port)
    wait(lambda: requests.get(config_url).json()["framing"]["keep_alive_period"] == keep_alive_period)

    environment.framing_options = {
        "keep_alive_period": keep_alive_period,
        "delay_before_command": delay_before_command,
        "suspending_path": suspending_path,
    }

    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class", params=["v3", "v4", "native_v3", "native_v4", "rpc"])
def test_environment_with_rpc(request):
    environment = init_environment_for_test_session(request.param)
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class")
def test_environment_for_yamr(request):
    environment = init_environment_for_test_session("yamr")
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class")
def test_environment_multicell(request):
    environment = init_environment_for_test_session(
        "native_multicell",
        env_options={"secondary_master_cell_count": 2})
    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class")
def test_environment_job_archive(request):
    if arcadia_interop.is_inside_distbuild():
        pytest.skip("porto is not available inside distbuild")

    environment = init_environment_for_test_session(
        "job_archive",
        env_options={"use_porto_for_servers": True},
        delta_node_config={
            "exec_agent": {
                "slot_manager": {
                    "enforce_job_control": True,
                    "job_environment": {
                        "type": "porto",
                    },
                },
                "statistics_reporter": {
                    "enabled": True,
                    "reporting_period": 10,
                    "min_repeat_delay": 10,
                    "max_repeat_delay": 10,
                }
            },
        },
        delta_scheduler_config={
            "scheduler": {
                "enable_job_reporter": True,
                "enable_job_spec_reporter": True,
            },
        },
        need_suid=True
    )

    sync_create_cell()
    init_operation_archive.create_tables_latest_version(yt, override_tablet_cell_bundle="default")

    request.addfinalizer(lambda: environment.cleanup())

    return environment

@pytest.fixture(scope="class", params=["v3", "v4", "native_v3", "native_v4", "rpc"])
def test_environment_with_porto(request):
    if arcadia_interop.is_inside_distbuild():
        pytest.skip("porto is not available inside distbuild")

    environment = init_environment_for_test_session(
        request.param,
        env_options={"use_porto_for_servers": True},
        delta_node_config={
            "exec_agent": {
                "slot_manager": {
                    "enforce_job_control": True,
                    "job_environment": {
                        "type": "porto",
                    },
                },
                "test_poll_job_shell": True,
            }
        },
        need_suid=True
    )

    request.addfinalizer(lambda: environment.cleanup())
    return environment

@pytest.fixture(scope="class", params=["v4", "rpc"])
def test_environment_with_increased_memory(request):
    environment = init_environment_for_test_session(
        request.param,
        env_options=dict(jobs_memory_limit=8*GB),
    )

    request.addfinalizer(lambda: environment.cleanup())
    return environment

# TODO(ignat): fix this copypaste from yt_env_setup
def _remove_operations():
    if yt.get("//sys/scheduler/instances/@count") == 0:
        return

    operation_from_orchid = []
    try:
        operation_from_orchid = [op_id for op_id in yt.list("//sys/scheduler/orchid/scheduler/operations")
                                 if not op_id.startswith("*")]
    except yt.YtError as err:
        print(format_error(err), file=sys.stderr)

    for operation_id in operation_from_orchid:
        try:
            yt.abort_operation(operation_id)
        except yt.YtError as err:
            print(format_error(err), file=sys.stderr)

    yt.remove("//sys/operations/*")

def _remove_objects():
    TYPES = [
        "accounts",
        "users",
        "groups",
        "racks",
        "data_centers",
        "tablet_cells",
        "tablet_cell_bundles",
    ]

    object_ids_to_remove = []
    object_ids_to_check = []
    for type in TYPES:

        objects = yt.list("//sys/" + ("account_tree" if type == "accounts" else type),
                          attributes=["id", "builtin", "life_stage"])
        for object in objects:
            if object.attributes["builtin"]:
                continue
            if type == "users" and str(object) == "application_operations":
                continue
            if type == "accounts" and str(object) == "operations_archive":
                continue

            id = object.attributes["id"]
            if type == "tablet_cells":
                try:
                    if any([yt.get("#" + tablet_id + "/@table_path").startswith("//sys/operations_archive")
                           for tablet_id in yt.get("#" + id + "/@tablet_ids")]):
                        continue
                except yt.YtError:
                    pass

            object_ids_to_check.append(id)
            if object.attributes["life_stage"] == "creation_committed":
                object_ids_to_remove.append(id)

    for id in object_ids_to_remove:
        yt.remove("#" + id, force=True, recursive=True)

    for id in object_ids_to_check:
        wait(lambda: not yt.exists("#" + id))

def test_method_teardown():
    if yt.config["backend"] == "proxy":
        assert yt.config["proxy"]["url"].startswith("localhost")

    for tx in yt.list("//sys/transactions", attributes=["title"]):
        title = tx.attributes.get("title", "")
        if "Scheduler lock" in title:
            continue
        if "Controller agent incarnation" in title:
            continue
        if "Lease for" in title:
            continue
        if "Prerequisite for" in title:
            continue
        try:
            yt.abort_transaction(tx)
        except:
            pass

    yt.remove(TEST_DIR, recursive=True, force=True)
    yt.remove("//tmp/*", recursive=True)

    _remove_operations()
    _remove_objects()

def _yt_env(request, test_environment):
    """ YT cluster fixture.
        Uses test_environment fixture.
        Starts YT cluster once per session but checks its health before each test function.
    """
    test_environment.check_liveness()
    test_environment.reload_global_configuration()
    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment

@pytest.fixture(scope="function")
def yt_env(request, test_environment):
    return _yt_env(request, test_environment)

@pytest.fixture(scope="function")
def yt_env_with_framing(request, test_environment_with_framing):
    return _yt_env(request, test_environment_with_framing)

@pytest.fixture(scope="function")
def yt_env_with_rpc(request, test_environment_with_rpc):
    return _yt_env(request, test_environment_with_rpc)

@pytest.fixture(scope="function")
def test_dynamic_library(request, yt_env):
    if not which("g++"):
        raise RuntimeError("g++ not found")
    libs_dir = os.path.join(yt_env.env.path, "yt_test_dynamic_library")
    makedirp(libs_dir)

    get_number_lib = get_test_file_path("getnumber.cpp")
    subprocess.check_call(["g++", get_number_lib, "-shared", "-fPIC", "-o", os.path.join(libs_dir, "libgetnumber.so")])

    dependant_lib = get_test_file_path("yt_test_lib.cpp")
    dependant_lib_output = os.path.join(libs_dir, "yt_test_dynamic_library.so")
    subprocess.check_call(["g++", dependant_lib, "-shared", "-o", dependant_lib_output,
                           "-L", libs_dir, "-l", "getnumber", "-fPIC"])

    # Adding this pseudo-module to sys.modules and ensuring it will be collected with
    # its dependency (libgetnumber.so)
    module = imp.new_module("yt_test_dynamic_library")
    module.__file__ = dependant_lib_output
    sys.modules["yt_test_dynamic_library"] = module

    def finalizer():
        del sys.modules["yt_test_dynamic_library"]

    request.addfinalizer(finalizer)
    return libs_dir, "libgetnumber.so"

@pytest.fixture(scope="function")
def config(yt_env):
    """ Test environment startup config fixture
        Used in tests to restore config after changes.
    """
    return deepcopy(yt_env.config)

@pytest.fixture(scope="function")
def yt_env_for_yamr(request, test_environment_for_yamr):
    """ YT cluster fixture for Yamr mode tests.
        Uses test_environment_for_yamr fixture.
        Starts YT cluster once per session but checks its health
        before each test function.
    """
    test_environment_for_yamr.check_liveness()
    test_environment_for_yamr.reload_global_configuration()

    yt.set_yamr_mode()
    yt.config["yamr_mode"]["treat_unexisting_as_empty"] = False
    if not yt.exists("//sys/empty_yamr_table"):
        yt.create("table", "//sys/empty_yamr_table", recursive=True)
    if not yt.is_sorted("//sys/empty_yamr_table"):
        yt.run_sort("//sys/empty_yamr_table", "//sys/empty_yamr_table", sort_by=["key", "subkey"])
    yt.config["yamr_mode"]["treat_unexisting_as_empty"] = True
    yt.config["default_value_of_raw_option"] = True

    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment_for_yamr

@pytest.fixture(scope="function")
def yt_env_multicell(request, test_environment_multicell):
    """ YT cluster fixture for tests with multiple cells.
    """
    test_environment_multicell.check_liveness()
    test_environment_multicell.reload_global_configuration()
    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment_multicell

@pytest.fixture(scope="function")
def yt_env_job_archive(request, test_environment_job_archive):
    """ YT cluster fixture for tests that require job archive
    """
    test_environment_job_archive.check_liveness()
    test_environment_job_archive.reload_global_configuration()
    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment_job_archive

@pytest.fixture(scope="function")
def yt_env_with_porto(request, test_environment_with_porto):
    """ YT cluster fixture for tests that require "porto" instead of "cgroups"
    """
    test_environment_with_porto.check_liveness()
    test_environment_with_porto.reload_global_configuration()
    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment_with_porto

@pytest.fixture(scope="function")
def yt_env_with_increased_memory(request, test_environment_with_increased_memory):
    test_environment_with_increased_memory.check_liveness()
    test_environment_with_increased_memory.reload_global_configuration()
    yt.mkdir(TEST_DIR, recursive=True)
    request.addfinalizer(test_method_teardown)
    return test_environment_with_increased_memory

@pytest.fixture(scope="function")
def job_events(request):
    return create_job_events()


# TODO(ignat): fix copy/paste from integration tests.
