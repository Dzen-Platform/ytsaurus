import configs

from yt.common import update
import yt.yson as yson

import logging
import os
import re
import time
import signal
import socket
import shutil
import subprocess
import sys
import simplejson as json
import collections


try:
    from unittest.util import unorderable_list_difference
except ImportError:
    def unorderable_list_difference(expected, actual, ignore_duplicate=False):
        """Same behavior as sorted_list_difference but
        for lists of unorderable items (like dicts).

        As it does a linear search per item (remove) it
        has O(n*n) performance.
        """
        missing = []
        unexpected = []
        while expected:
            item = expected.pop()
            try:
                actual.remove(item)
            except ValueError:
                missing.append(item)
            if ignore_duplicate:
                for lst in expected, actual:
                    try:
                        while True:
                            lst.remove(item)
                    except ValueError:
                        pass
        if ignore_duplicate:
            while actual:
                item = actual.pop()
                unexpected.append(item)
                try:
                    while True:
                        actual.remove(item)
                except ValueError:
                    pass
            return missing, unexpected

        # anything left in actual is unexpected
        return missing, actual


try:
    from unittest.case import _AssertRaisesContext
except ImportError:
    class _AssertRaisesContext(object):
        """A context manager used to implement TestCase.assertRaises* methods."""

        def __init__(self, expected, test_case, expected_regexp=None):
            self.expected = expected
            self.failureException = test_case.failureException
            self.expected_regexp = expected_regexp

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc_value, tb):
            if exc_type is None:
                try:
                    exc_name = self.expected.__name__
                except AttributeError:
                    exc_name = str(self.expected)
                raise self.failureException(
                    "{0} not raised".format(exc_name))
            if not issubclass(exc_type, self.expected):
                # let unexpected exceptions pass through
                return False
            self.exception = exc_value # store for later retrieval
            if self.expected_regexp is None:
                return True

            expected_regexp = self.expected_regexp
            if isinstance(expected_regexp, basestring):
                expected_regexp = re.compile(expected_regexp)
            if not expected_regexp.search(str(exc_value)):
                raise self.failureException('"%s" does not match "%s"' %
                         (expected_regexp.pattern, str(exc_value)))
            return True


try:
    from collections import Counter
except ImportError:
    def Counter(iterable):
        result = {}
        for item in iterable:
            result[item] = result.get(item, 0) + 1
        return result


def init_logging(node, path, name):
    for key, suffix in [('file', '.log'), ('raw', '.debug.log')]:
        node['writers'][key]['file_name'] = os.path.join(path, name + suffix)

def write_config(config, filename):
    with open(filename, 'wt') as f:
        f.write(yson.dumps(config, indent = '    '))

def write_with_flush(data):
    sys.stdout.write(data)
    sys.stdout.flush()

class YTEnv(object):
    failureException = Exception

    NUM_MASTERS = 3
    NUM_NODES = 5
    START_SCHEDULER = False
    START_PROXY = False

    DELTA_MASTER_CONFIG = {}
    DELTA_NODE_CONFIG = {}
    DELTA_SCHEDULER_CONFIG = {}

    # to be redefiened in successors
    def modify_master_config(self, config):
        pass

    # to be redefined in successors
    def modify_node_config(self, config):
        pass

    # to be redefined in successors
    def modify_scheduler_config(self, config):
        pass

    def set_environment(self, path_to_run, pids_filename, ports=None, supress_yt_output=False):
        logging.basicConfig(format='%(message)s')

        self.supress_yt_output = supress_yt_output
        self.path_to_run = os.path.abspath(path_to_run)
        if os.path.exists(self.path_to_run):
            shutil.rmtree(self.path_to_run, ignore_errors=True)
        try:
            os.makedirs(self.path_to_run)
        except:
            pass

        self._pids_filename = pids_filename
        self._kill_previously_run_services()
        self._ports = {
            "master": 8001,
            "node": 7001,
            "scheduler": 8101,
            "proxy": 8080,
            "proxy_log": 8081}
        if ports is not None:
            self._ports.update(ports)

        logging.info('Setting up configuration with %s masters, %s nodes, %s schedulers.' %
                     (self.NUM_MASTERS, self.NUM_NODES, self.START_SCHEDULER))
        logging.info('SANDBOX_DIR is %s', self.path_to_run)

        self.process_to_kill = []

        if self.NUM_MASTERS == 0:
            logging.info("Do nothing, because we have 0 masters")
            return

        try:
            logging.info("Configuring...")
            self._run_masters()
            self._run_schedulers()
            self._run_nodes()
            self._prepare_driver()
            self._run_proxy()
        except:
            self.clear_environment()
            raise

    def kill_process(self, proc, name):
        ok = True
        message = ""
        proc.poll()
        if proc.returncode is not None:
            ok = False
            message += '%s (pid %d) is already terminated with exit status %d\n' % (name, proc.pid, proc.returncode)
        else:
            os.killpg(proc.pid, signal.SIGKILL)

        time.sleep(0.250)

        # now try to kill unkilled process
        for i in xrange(50):
            proc.poll()
            if proc.returncode is not None:
                break
            logging.warning('%s (pid %d) was not killed by the kill command' % (name, proc.pid))

            os.killpg(proc.pid, signal.SIGKILL)
            time.sleep(0.100)

        if proc.returncode is None:
            ok = False
            message += 'Alarm! %s (pid %d) was not killed after 50 iterations\n' % (name, proc.pid)
        return message, ok

    def clear_environment(self):
        ok = True
        message = ""
        for p, name in self.process_to_kill:
            p_message, p_ok = self.kill_process(p, name)
            if not p_ok: ok = False
            message += p_message

        assert ok, message

    def _append_pid(self, pid):
        self.pids_file.write(str(pid) + '\n')
        self.pids_file.flush();

    def _run(self, args, name, timeout=0.5):
        if self.supress_yt_output:
            stdout = open("/dev/null", "w")
            stderr = open("/dev/null", "w")
        else:
            stdout = sys.stdout
            stderr = sys.stderr
        p = subprocess.Popen(args, shell=False, close_fds=True, preexec_fn=os.setsid,
                             stdout=stdout, stderr=stderr)
        self.process_to_kill.append((p, name))
        self._append_pid(p.pid)

        time.sleep(timeout)
        if p.poll():
            print >>sys.stderr, "Process %s unexpectedly terminated." % name
            print >>sys.stderr, "Check that there are no other incarnations of this process."
            assert False, "Process unexpectedly terminated"

    def _run_ytserver(self, service_name, configs):
        for i in xrange(len(configs)):
            self._run([
                'ytserver', "--" + service_name,
                '--config', configs[i]],
                "%s-%d" % (service_name, i))

    def _kill_previously_run_services(self):
        if os.path.exists(self._pids_filename):
            with open(self._pids_filename, 'rt') as f:
                for pid in map(int, f.xreadlines()):
                    try:
                        os.killpg(pid, signal.SIGKILL)
                    except OSError:
                        pass

        dirname = os.path.dirname(self._pids_filename)
        if not os.path.exists(dirname):
            os.makedirs(dirname)
        self.pids_file = open(self._pids_filename, 'wt')


    def _run_masters(self, prepare_files=True):
        if self.NUM_MASTERS == 0: return

        short_hostname = socket.gethostname()
        hostname = socket.gethostbyname_ex(short_hostname)[0]

        self._master_addresses = ["%s:%s" % (hostname, self._ports["master"] + 2*i)
                                  for i in xrange(self.NUM_MASTERS)]
        self._master_configs = []

        logs = []
        config_paths = []

        if prepare_files:
            os.mkdir(os.path.join(self.path_to_run, 'master'))

        current_port = self._ports["master"]
        for i in xrange(self.NUM_MASTERS):
            config = configs.get_master_config()

            current = os.path.join(self.path_to_run, 'master', str(i))
            if prepare_files:
                os.mkdir(current)

            config['meta_state']['cell']['rpc_port'] = current_port
            config['monitoring_port'] = current_port + 1
            current_port += 2

            config['meta_state']['cell']['addresses'] = self._master_addresses
            config['meta_state']['changelogs']['path'] = \
                os.path.join(current, 'logs')
            config['meta_state']['snapshots']['path'] = \
                    os.path.join(current, 'snapshots')
            init_logging(config['logging'], current, 'master-' + str(i))

            self.modify_master_config(config)
            update(config, self.DELTA_MASTER_CONFIG)

            logs.append(config['logging']['writers']['file']['file_name'])

            self._master_configs.append(config)

            config_path = os.path.join(current, 'master_config.yson')
            if prepare_files:
                write_config(config, config_path)
            config_paths.append(config_path)


        self._run_ytserver('master', config_paths)

        def masters_ready():
            good_marker = "World initialization completed"
            bad_marker = "Active quorum lost"

            master_id = 0
            for logging_file in logs:
                if not os.path.exists(logging_file): continue

                for line in reversed(open(logging_file).readlines()):
                    if bad_marker in line: continue
                    if good_marker in line:
                        self.leader_log = logging_file
                        self.leader_id = master_id
                        return True
                master_id += 1
            return False

        self._wait_for(masters_ready, name = "masters")
        logging.info('(Leader is: %d)', self.leader_id)


    def _run_nodes(self, prepare_files=True):
        if self.NUM_NODES == 0: return

        self.node_configs = []

        config_paths = []

        if prepare_files:
            os.mkdir(os.path.join(self.path_to_run, 'node'))

        current_user = 10000;
        current_port = self._ports["node"]
        for i in xrange(self.NUM_NODES):
            config = configs.get_node_config()

            current = os.path.join(self.path_to_run, 'node', str(i))
            os.mkdir(current)

            config['rpc_port'] = current_port
            config['monitoring_port'] = current_port + 1
            current_port += 2

            config['masters']['addresses'] = self._master_addresses
            config['data_node']['cache_location']['path'] = \
                os.path.join(current, 'chunk_cache')
            config['data_node']['store_locations'].append(
                {'path': os.path.join(current, 'chunk_store'),
                 'low_watermark' : 0,
                 'high_watermark' : 0})
            config['exec_agent']['slot_manager']['start_uid'] = current_user
            config['exec_agent']['slot_manager']['path'] = \
                os.path.join(current, 'slot')

            current_user += config['exec_agent']['job_controller']['resource_limits']['slots'] + 1

            init_logging(config['logging'], current, 'node-%d' % i)
            init_logging(config['exec_agent']['job_proxy_logging'], current, 'job_proxy-%d' % i)

            self.modify_node_config(config)
            update(config, self.DELTA_NODE_CONFIG)

            self.node_configs.append(config)

            config_path = os.path.join(current, 'node_config.yson')
            write_config(config, config_path)
            config_paths.append(config_path)

        self._run_ytserver('node', config_paths)


        def all_nodes_ready():
            nodes_status = {}

            scheduler_good_marker = re.compile(r".*Node online.*")
            good_marker = re.compile(r".*Node online .*NodeId: (\d+).*")
            bad_marker = re.compile(r".*Node unregistered .*NodeId: (\d+).*")

            def update_status(marker, line, status, value):
                match = marker.match(line)
                if match:
                    node_id = match.group(1)
                    if node_id not in status:
                        status[node_id] = value

            for line in reversed(open(self.leader_log).readlines()):
                update_status(good_marker, line, nodes_status, True)
                update_status(bad_marker, line, nodes_status, False)

            scheduler_ready = False
            if self.scheduler_log is not None:
                ready = 0
                for line in reversed(open(self.scheduler_log).readlines()):
                    if scheduler_good_marker.match(line):
                        ready += 1
                scheduler_ready = (ready == self.NUM_NODES)
            else:
                scheduler_ready = True

            if len(nodes_status) != self.NUM_NODES: return False
            return all(nodes_status.values()) and scheduler_ready

        self._wait_for(all_nodes_ready, name = "nodes",
                       max_wait_time = max(self.NUM_NODES * 6.0, 20))


    def _run_schedulers(self):
        self.scheduler_log = None
        if not self.START_SCHEDULER: return

        current = os.path.join(self.path_to_run, 'scheduler')
        os.mkdir(current)

        config = configs.get_scheduler_config()
        config['masters']['addresses'] = self._master_addresses
        init_logging(config['logging'], current, 'scheduler')

        current_port = self._ports["scheduler"]
        config['rpc_port'] = current_port
        config['monitoring_port'] = current_port + 1

        config['scheduler']['snapshot_temp_path'] = os.path.join(current, 'snapshots')

        self.modify_scheduler_config(config)
        update(config, self.DELTA_SCHEDULER_CONFIG)
        config_path = os.path.join(current, 'scheduler_config.yson')
        write_config(config, config_path)

        self._run_ytserver('scheduler', [config_path])

        def scheduler_ready():
            good_marker = 'Master connected'

            log = config['logging']['writers']['file']['file_name']
            self.scheduler_log = log
            if not os.path.exists(log): return False
            for line in reversed(open(log).readlines()):
                if good_marker in line:
                    return True
            return False

        self._wait_for(scheduler_ready, name = "scheduler")

    def _prepare_driver(self):
        config = configs.get_driver_config()
        config['masters']['addresses'] = self._master_addresses
        config_path = os.path.join(self.path_to_run, 'driver_config.yson')
        write_config(config, config_path)
        os.environ['YT_CONFIG'] = config_path

    def _run_proxy(self):
        if not self.START_PROXY: return

        current = os.path.join(self.path_to_run, "proxy")
        os.mkdir(current)

        driver_config = configs.get_driver_config()
        driver_config['masters']['addresses'] = self._master_addresses
        init_logging(driver_config['logging'], current, 'node')

        proxy_config = configs.get_proxy_config()
        proxy_config['proxy']['logging'] = driver_config['logging']
        del driver_config['logging']
        proxy_config['proxy']['driver'] = driver_config
        proxy_config['port'] = self._ports["proxy"]
        proxy_config['log_port'] = self._ports["proxy_log"]

        config_path = os.path.join(self.path_to_run, 'proxy_config.json')
        with open(config_path, "w") as f:
            f.write(json.dumps(proxy_config))

        log = os.path.join(current, "http_application.log")
        self._run(['run_proxy.sh', "-c", config_path, "-l", log], "proxy", timeout=3.0)

        def started():
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.connect(('127.0.0.1', self._ports["proxy"]))
                s.shutdown(2)
                return True
            except:
                return False

        self._wait_for(started, name="proxy", max_wait_time=90)

    def _wait_for(self, condition, max_wait_time=20, sleep_quantum=0.5, name=""):
        current_wait_time = 0
        write_with_flush('Waiting for %s' % name)
        while current_wait_time < max_wait_time:
            write_with_flush('.')
            if condition():
                write_with_flush(' %s ready\n' % name)
                return
            time.sleep(sleep_quantum)
            current_wait_time += sleep_quantum
        assert False, "%s still not ready after %s seconds" % (name, max_wait_time)

    # Unittest is painfull to integrate, so we simply reimplement some methods
    def assertItemsEqual(self, actual_seq, expected_seq):
        # It is simplified version of the same method of unittest.TestCase
        try:
            actual = Counter(iter(actual_seq))
            expected = Counter(iter(expected_seq))
        except TypeError:
            # Unsortable items (example: set(), complex(), ...)
            actual = list(actual_seq)
            expected = list(expected_seq)
            missing, unexpected = unorderable_list_difference(expected, actual)
        else:
            if actual == expected:
                return
            missing = list(expected - actual)
            unexpected = list(actual - expected)

        assert not missing, 'Expected, but missing:\n    %s' % repr(missing)
        assert not unexpected, 'Unexpected, but present:\n    %s' % repr(unexpected)

    def assertEqual(self, actual, expected, msg=""):
        self.assertTrue(actual == expected, msg)

    def assertTrue(self, expr, msg=""):
        assert expr, msg

    def assertFalse(self, expr, msg=""):
        assert not expr, msg

    def assertRaises(self, excClass, callableObj=None, *args, **kwargs):
        context = _AssertRaisesContext(excClass, self)
        if callableObj is None:
            return context
        with context:
            callableObj(*args, **kwargs)
