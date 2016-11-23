import yt_commands

from yt.environment import YTInstance
from yt.common import makedirp, update, YtError
import yt_driver_bindings

import yt.yson as yson

import pytest

import gc
import os
import sys
import logging
import resource
import shutil
import stat
import subprocess
import uuid
import __builtin__
from distutils.spawn import find_executable
from time import sleep, time
from threading import Thread

SANDBOX_ROOTDIR = os.environ.get("TESTS_SANDBOX", os.path.abspath('tests.sandbox'))
SANDBOX_STORAGE_ROOTDIR = os.environ.get("TESTS_SANDBOX_STORAGE")
TOOLS_ROOTDIR = os.path.abspath('tools')

linux_only = pytest.mark.skipif('not sys.platform.startswith("linux")')
unix_only = pytest.mark.skipif('not sys.platform.startswith("linux") and not sys.platform.startswith("darwin")')

def skip_if_multicell(func):
    def wrapped_func(self, *args, **kwargs):
        if hasattr(self, "NUM_SECONDARY_MASTER_CELLS") and self.NUM_SECONDARY_MASTER_CELLS > 0:
            pytest.skip("This test does not support multicell mode")
        func(self, *args, **kwargs)
    return wrapped_func

def require_ytserver_root_privileges(func):
    def wrapped_func(self, *args, **kwargs):
        ytserver_path = find_executable("ytserver")
        ytserver_stat = os.stat(ytserver_path)
        if (ytserver_stat.st_mode & stat.S_ISUID) == 0:
            pytest.fail("This test requires a suid bit set for ytserver")
        if ytserver_stat.st_uid != 0:
            pytest.fail("This test requires ytserver being owned by root")
        func(self, *args, **kwargs)
    return wrapped_func

def require_enabled_core_dump(func):
    def wrapped_func(self, *args, **kwargs):
        rlimit_core = resource.getrlimit(resource.RLIMIT_CORE)
        if rlimit_core[0] == 0:
            pytest.skip("This test requires enabled core dump (how about 'ulimit -c unlimited'?)")
        func(self, *args, **kwargs)
    return wrapped_func

def resolve_test_paths(name):
    path_to_sandbox = os.path.join(SANDBOX_ROOTDIR, name)
    path_to_environment = os.path.join(path_to_sandbox, 'run')
    return path_to_sandbox, path_to_environment

def wait(predicate):
    for _ in xrange(100):
        if predicate():
            return
        sleep(1.0)
    pytest.fail("wait failed")

def make_schema(columns, **attributes):
    schema = yson.YsonList(columns)
    for attr, value in attributes.items():
        schema.attributes[attr] = value
    return schema

def _pytest_finalize_func(environment, process_call_args):
    print >>sys.stderr, 'Process run by command "{0}" is dead!'.format(" ".join(process_call_args))
    environment.stop()

    print >>sys.stderr, "Killing pytest process"
    os._exit(42)

class Checker(Thread):
    def __init__(self, check_function):
        super(Checker, self).__init__()
        self._check_function = check_function
        self._active = None

    def start(self):
        self._active = True
        super(Checker, self).start()

    def run(self):
        while self._active:
            now = time()
            self._check_function()
            delta = time() - now
            if delta > 0.1:
                print >>sys.stderr, "check takes %lf seconds" % delta
            sleep(1.0)

    def stop(self):
        self._active = False
        self.join()

class YTEnvSetup(object):
    NUM_MASTERS = 3
    NUM_NONVOTING_MASTERS = 0
    NUM_SECONDARY_MASTER_CELLS = 0
    START_SECONDARY_MASTER_CELLS = True
    NUM_NODES = 5
    NUM_SCHEDULERS = 0

    DELTA_DRIVER_CONFIG = {}
    DELTA_MASTER_CONFIG = {}
    DELTA_NODE_CONFIG = {}
    DELTA_SCHEDULER_CONFIG = {}

    # To be redefined in successors
    @classmethod
    def modify_master_config(cls, config):
        pass

    @classmethod
    def modify_scheduler_config(cls, config):
        pass

    @classmethod
    def modify_node_config(cls, config):
        pass

    @classmethod
    def setup_class(cls, test_name=None):
        logging.basicConfig(level=logging.INFO)

        if test_name is None:
            test_name = cls.__name__
        cls.test_name = test_name
        path_to_test = os.path.join(SANDBOX_ROOTDIR, test_name)

        # Should create before env start for correct behaviour of teardown.
        cls.liveness_checker = None

        cls.path_to_test = path_to_test
        # For running in parallel
        cls.run_id = "run_" + uuid.uuid4().hex[:8]
        cls.path_to_run = os.path.join(path_to_test, cls.run_id)

        cls.Env = YTInstance(
            cls.path_to_run,
            master_count=cls.NUM_MASTERS,
            nonvoting_master_count=cls.NUM_NONVOTING_MASTERS,
            secondary_master_cell_count=cls.NUM_SECONDARY_MASTER_CELLS,
            node_count=cls.NUM_NODES,
            scheduler_count=cls.NUM_SCHEDULERS,
            kill_child_processes=True,
            port_locks_path=os.path.join(SANDBOX_ROOTDIR, "ports"),
            fqdn="localhost",
            modify_configs_func=cls.apply_config_patches)
        cls.Env.start(start_secondary_master_cells=cls.START_SECONDARY_MASTER_CELLS)

        yt_commands.path_to_run_tests = cls.path_to_run

        if cls.Env.configs["driver"]:
            secondary_driver_configs = [cls.Env.configs["driver_secondary_{0}".format(i)]
                                        for i in xrange(cls.NUM_SECONDARY_MASTER_CELLS)]
            yt_commands.init_driver(cls.Env.configs["driver"], secondary_driver_configs)
            yt_commands.is_multicell = (cls.NUM_SECONDARY_MASTER_CELLS > 0)
            yt_driver_bindings.configure_logging(cls.Env.driver_logging_config)

        # To avoid strange hangups.
        if cls.NUM_MASTERS > 0:
            cls.liveness_checker = Checker(lambda: cls.Env.check_liveness(callback_func=_pytest_finalize_func))
            cls.liveness_checker.daemon = True
            cls.liveness_checker.start()

    @classmethod
    def apply_config_patches(cls, configs, ytserver_version):
        for tag in [configs["master"]["primary_cell_tag"]] + configs["master"]["secondary_cell_tags"]:
            for config in configs["master"][tag]:
                update(config, cls.DELTA_MASTER_CONFIG)
                cls.modify_master_config(config)
        for config in configs["scheduler"]:
            update(config, cls.DELTA_SCHEDULER_CONFIG)
            cls.modify_scheduler_config(config)
        for config in configs["node"]:
            update(config, cls.DELTA_NODE_CONFIG)
            cls.modify_node_config(config)
        for config in configs["driver"].values():
            update(config, cls.DELTA_DRIVER_CONFIG)

    @classmethod
    def teardown_class(cls):
        if cls.liveness_checker is not None:
            cls.liveness_checker.stop()

        cls.Env.stop()
        yt_commands.driver = None
        gc.collect()

        if not os.path.exists(cls.path_to_run):
            return

        if SANDBOX_STORAGE_ROOTDIR is not None:
            makedirp(SANDBOX_STORAGE_ROOTDIR)

            # XXX(asaitgalin): Ensure tests running user has enough permissions to manipulate YT sandbox.
            chown_command = ["sudo", "chown", "-R", "{0}:{1}".format(os.getuid(), os.getgid()), cls.path_to_run]
            p = subprocess.Popen(chown_command, stderr=subprocess.PIPE)
            _, stderr = p.communicate()
            if p.returncode != 0:
                print >>sys.stderr, stderr
                raise subprocess.CalledProcessError(p.returncode, " ".join(chown_command))

            # XXX(dcherednik): Detete named pipes
            subprocess.check_call(["find", cls.path_to_run, "-type", "p", "-delete"])

            destination_path = os.path.join(SANDBOX_STORAGE_ROOTDIR, cls.test_name, cls.run_id)
            if os.path.exists(destination_path):
                shutil.rmtree(destination_path)

            shutil.move(cls.path_to_run, destination_path)

    def setup_method(self, method):
        if self.NUM_MASTERS > 0:
            self.transactions_at_start = set(yt_commands.get_transactions())
            self.wait_for_nodes()
            self.wait_for_chunk_replicator()

    def teardown_method(self, method):
        self.Env.check_liveness(callback_func=_pytest_finalize_func)
        if self.NUM_MASTERS > 0:
            self._abort_transactions()
            yt_commands.set('//tmp', {})
            self._remove_operations()
            yt_commands.gc_collect()
            yt_commands.clear_metadata_caches()
            self._reset_nodes()
            self._reenable_chunk_replicator()
            self._remove_accounts()
            self._remove_users()
            self._remove_groups()
            self._remove_tablet_cells()
            self._remove_racks()
            self._remove_pools()

            yt_commands.gc_collect()

    def set_node_banned(self, address, flag):
        yt_commands.set("//sys/nodes/%s/@banned" % address, flag)
        # Give it enough time to register or unregister the node
        sleep(1.0)
        if flag:
            assert yt_commands.get("//sys/nodes/%s/@state" % address) == "offline"
            print "Node %s is banned" % address
        else:
            assert yt_commands.get("//sys/nodes/%s/@state" % address) == "online"
            print "Node %s is unbanned" % address

    def wait_for_nodes(self):
        print "Waiting for nodes to become online..."
        wait(lambda: all(n.attributes["state"] == "online" for n in yt_commands.ls("//sys/nodes", attributes=["state"])))

    def wait_for_chunk_replicator(self):
        print "Waiting for chunk replicator to become enabled..."
        wait(lambda: yt_commands.get("//sys/@chunk_replicator_enabled"))

    def wait_for_cells(self):
        print "Waiting for tablet cells to become healthy..."
        wait(lambda: all(c.attributes["health"] == "good" for c in yt_commands.ls("//sys/tablet_cells", attributes=["health"])))

    def sync_create_cells(self, peer_count, cell_count):
        for _ in xrange(cell_count):
            yt_commands.create_tablet_cell(peer_count)
        self.wait_for_cells()

    def wait_for_tablet_state(self, path, states):
        print "Waiting for tablets to become %s..." % ", ".join(str(state) for state in states)
        wait(lambda: all(any(x["state"] == state for state in states) for x in yt_commands.get(path + "/@tablets")))

    def wait_until_sealed(self, path):
        wait(lambda: yt_commands.get(path + "/@sealed"))

    def _wait_for_tablets(self, path, state, **kwargs):
        tablet_count = yt_commands.get(path + '/@tablet_count')
        first_tablet_index = kwargs.get("first_tablet_index", 0)
        last_tablet_index = kwargs.get("last_tablet_index", tablet_count - 1)
        wait(lambda: all(x["state"] == state for x in yt_commands.get(path + "/@tablets")[first_tablet_index:last_tablet_index + 1]))

    def sync_mount_table(self, path, **kwargs):
        yt_commands.mount_table(path, **kwargs)

        print "Waiting for tablets to become mounted..."
        self._wait_for_tablets(path, "mounted", **kwargs)

    def sync_unmount_table(self, path, **kwargs):
        yt_commands.unmount_table(path, **kwargs)

        print "Waiting for tablets to become unmounted..."
        self._wait_for_tablets(path, "unmounted", **kwargs)

    def sync_compact_table(self, path):
        self.sync_unmount_table(path)
        chunk_ids = __builtin__.set(yt_commands.get(path + "/@chunk_ids"))
        yt_commands.set(path + "/@forced_compaction_revision", yt_commands.get(path + "/@revision"))
        self.sync_mount_table(path)

        print "Waiting for tablets to become compacted..."
        wait(lambda: len(chunk_ids.intersection(__builtin__.set(yt_commands.get(path + "/@chunk_ids")))) == 0)

    def _abort_transactions(self):
         for tx in yt_commands.ls("//sys/transactions", attributes=["title"]):
            title = tx.attributes.get("title", "")
            id = str(tx)
            if "Scheduler lock" in title:
                continue
            if "Lease for node" in title:
                continue
            try:
                yt_commands.abort_transaction(id)
            except:
                pass

    def _reset_nodes(self):
        nodes = yt_commands.ls("//sys/nodes", attributes=["banned", "resource_limits_overrides"])
        for node in nodes:
            node_name = str(node)
            if node.attributes["banned"]:
                yt_commands.set("//sys/nodes/%s/@banned" % node_name, False)
            if node.attributes["resource_limits_overrides"] != {}:
                yt_commands.set("//sys/nodes/%s/@resource_limits_overrides" % node_name, {})

    def _remove_operations(self):
        try:
            for operation_id in yt_commands.ls("//sys/scheduler/orchid/scheduler/operations"):
                yt_commands.abort_op(operation_id)
        except YtError:
            pass
        for operation in yt_commands.ls("//sys/operations"):
            yt_commands.remove("//sys/operations/" + operation, recursive=True)

    def _reenable_chunk_replicator(self):
        if yt_commands.exists("//sys/@disable_chunk_replicator"):
            yt_commands.remove("//sys/@disable_chunk_replicator")

    def _remove_accounts(self):
        accounts = yt_commands.ls("//sys/accounts", attributes=["builtin", "resource_usage"])
        for account in accounts:
            if not account.attributes["builtin"]:
                print >>sys.stderr, account.attributes["resource_usage"]
                yt_commands.remove_account(str(account))

    def _remove_users(self):
        users = yt_commands.ls("//sys/users", attributes=["builtin"])
        for user in users:
            if not user.attributes["builtin"]:
                yt_commands.remove_user(str(user))

    def _remove_groups(self):
        groups = yt_commands.ls("//sys/groups", attributes=["builtin"])
        for group in groups:
            if not group.attributes["builtin"]:
                yt_commands.remove_group(str(group))

    def _remove_tablet_cells(self):
        cells = yt_commands.get_tablet_cells()
        for id in cells:
            yt_commands.remove_tablet_cell(id)

    def _remove_racks(self):
        racks = yt_commands.get_racks()
        for rack in racks:
            yt_commands.remove_rack(rack)

    def _remove_pools(self):
        yt_commands.remove("//sys/pools/*")

    def _find_ut_file(self, file_name):
        ytserver_path = find_executable("ytserver")
        assert ytserver_path is not None
        unittests_path = os.path.join(os.path.dirname(ytserver_path), "..", "yt", "unittests")
        assert os.path.exists(unittests_path)
        result_path = os.path.join(unittests_path, file_name)
        assert os.path.exists(result_path)
        return result_path

