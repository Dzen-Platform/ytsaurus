from yt_env_setup import wait, YTEnvSetup
from yt_commands import *
import yt.environment.init_operation_archive as init_operation_archive
from yt.wrapper.common import uuid_hash_pair
from yt.common import date_string_to_timestamp_mcs

import __builtin__
import datetime
import itertools
import pytest
import shutil

def id_to_parts(id):
    id_parts = id.split("-")
    id_hi = long(id_parts[2], 16) << 32 | int(id_parts[3], 16)
    id_lo = long(id_parts[0], 16) << 32 | int(id_parts[1], 16)
    return id_hi, id_lo

class TestGetJobStderr(YTEnvSetup):
    NUM_MASTERS = 1 
    NUM_NODES = 3 
    NUM_SCHEDULERS = 1 
    USE_DYNAMIC_TABLES = True

    def setup(self):
        self.sync_create_cells(1)
        init_operation_archive.create_tables_latest_version(self.Env.create_native_client())

    def teardown(self):
        remove("//sys/operations_archive")

    def test_get_job_stderr(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"foo": "bar"}, {"foo": "baz"}, {"foo": "qux"}])

        op = map(
            dont_track=True,
            wait_for_jobs=True,
            label="get_job_stderr",
            in_="//tmp/t1",
            out="//tmp/t2",
            precommand="echo STDERR-OUTPUT >&2",
            command="cat",
            spec={
                "mapper": {
                    "input_format": "json",
                    "output_format": "json"
                }
            })

        job_id = op.jobs[0]
        res = get_job_stderr(op.id, job_id)
        assert res == "STDERR-OUTPUT\n"
        op.resume_jobs()
        op.track()
        res = get_job_stderr(op.id, job_id)
        assert res == "STDERR-OUTPUT\n"

        stderrs_archive_path = "//sys/operations_archive/stderrs"

        op_id_hi, op_id_lo = id_to_parts(op.id)
        id_hi, id_lo = id_to_parts(job_id)

        row = {}
        row["operation_id_hi"] = yson.YsonUint64(op_id_hi)
        row["operation_id_lo"] = yson.YsonUint64(op_id_lo)
        row["job_id_hi"] = yson.YsonUint64(id_hi)
        row["job_id_lo"] = yson.YsonUint64(id_lo)
        row["stderr"] =  res

        insert_rows(stderrs_archive_path, [row])

        remove("//sys/operations/{}".format(op.id))

        res = get_job_stderr(op.id, job_id)
        assert res == "STDERR-OUTPUT\n"
        self.sync_unmount_table(stderrs_archive_path)
        remove(stderrs_archive_path) 
