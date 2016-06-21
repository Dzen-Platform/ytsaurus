import pytest
import sys

from yt_env_setup import YTEnvSetup, unix_only
from yt_commands import *


##################################################################

"""
This test only works when suid bit is set.
"""

def check_memory_limit(op):
    jobs_path = "//sys/operations/" + op.id + "/jobs"
    for job_id in ls(jobs_path):
        inner_errors = get(jobs_path + "/" + job_id + "/@error/inner_errors")
        assert "Memory limit exceeded" in inner_errors[0]["message"]

class TestSchedulerMemoryLimits(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent" : {
            "enable_cgroups" : True,
            "supported_cgroups" : [ "cpuacct", "blkio", "memory", "cpu" ],
            "slot_manager" : {
                "enforce_job_control"    : True,
                "memory_watchdog_period" : 100
            }
        }
    }

    #pytest.mark.xfail(run = False, reason = "Set-uid-root before running.")
    @unix_only
    def test_map(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"value": "value", "subkey": "subkey", "key": "key", "a": "another"})

        mapper = \
"""
a = list()
while True:
    a.append(''.join(['xxx'] * 10000))
"""

        create("file", "//tmp/mapper.py")
        write_file("//tmp/mapper.py", mapper)

        create("table", "//tmp/t_out")

        op = map(dont_track=True,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.py",
            file="//tmp/mapper.py",
            spec={"max_failed_job_count": 5})

        # if all jobs failed then operation is also failed
        with pytest.raises(YtError):
            op.track()
        # ToDo: check job error messages.
        check_memory_limit(op)

    @unix_only
    def test_dirty_sandbox(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"value": "value", "subkey": "subkey", "key": "key", "a": "another"})

        create("table", "//tmp/t_out")

        command = "cat > /dev/null; mkdir ./tmpxxx; echo 1 > ./tmpxxx/f1; chmod 700 ./tmpxxx;"
        map(in_="//tmp/t_in", out="//tmp/t_out", command=command)


class TestMemoryReserveFactor(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent" : {
            "enable_cgroups" : True,
            "supported_cgroups" : [ "cpuacct", "blkio", "memory", "cpu" ],
            "slot_manager" : {
                "enforce_job_control"  : True,
                "memory_watchdog_period" : 100
            }
        }
    }

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "event_log": {
                "flush_period": 100
            },
            "user_job_success_rate_quantile_precision" : 0.05
        }
    }

    @unix_only
    def test_memory_reserve_factor(self):
        job_count = 30

        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"key" : i} for i in range(job_count)])

        mapper = \
"""
#!/usr/bin/python
from random import randint
def rndstr(n):
    s = ''
    for i in range(100):
        s += chr(randint(ord('a'), ord('z')))
    return s * (n // 100)

a = list()
while len(a) * 100000 < 7e7:
    a.append(rndstr(100000))

"""
        create("file", "//tmp/mapper.py")
        write_file("//tmp/mapper.py", mapper)
        set("//tmp/mapper.py/@executable", True)
        create("table", "//tmp/t_out")

        op = map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.py",
            file="//tmp/mapper.py",
            spec={"job_count" : job_count, "mapper" : {"memory_limit": 10**8, "user_slots": 1}})

        time.sleep(1)
        event_log = read_table("//sys/scheduler/event_log")
        last_memory_reserve = None
        for event in event_log:
            if event["event_type"] == "job_completed" and event["operation_id"] == op.id:
                print >>sys.stderr, event["job_id"], event["statistics"]["user_job"]["memory_reserve"]["sum"]
                last_memory_reserve = int(event["statistics"]["user_job"]["memory_reserve"]["sum"])
        assert not last_memory_reserve is None
        assert 6e7 <= last_memory_reserve <= 8e7

