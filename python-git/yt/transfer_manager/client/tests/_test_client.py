from yt.transfer_manager.client import TransferManager
from yt.wrapper.client import Yt
import yt.logger as logger

import time
import logging

logger.LOGGER.setLevel(logging.INFO)

"""Test file name starts with _ because we do not want to run it in Teamcity."""

def _wait_task(task_id, client):
    while True:
        info = client.get_task_info(task_id)
        assert info["state"] not in ["failed", "aborted"]
        if info["state"] in ["completed", "skipped"]:
            break

def test_copy_between_clusters(backend_url):
    client = TransferManager(url=backend_url)

    hahn_client = Yt(proxy="hahn")
    table = hahn_client.create_temp_table()
    hahn_client.write_table(table, ["a\tb\n", "c\td\n", "e\tf\n"], format="yamr", raw=True)

    client.add_task("hahn", table, "banach", "//tmp/tm_client_test_table", sync=True)

    task_id = client.add_task("hahn", table, "titan", "tmp/yt/tm_client_test_table",
                              params={"mr_user": "imgdev"})
    time.sleep(0.5)
    assert task_id in [task["id"] for task in client.get_tasks()]
    _wait_task(task_id, client)

    task_id = client.add_task("titan", "tmp/yt/tm_client_test_table", "hahn", "//tmp/tm_client_test_table",
                              sync=True, poll_period=10, params={"pool": "ignat"})

    assert client.get_task_info(task_id)["state"] == "completed"
    assert client.get_task_info(task_id)["pool"] == "ignat"
    assert hahn_client.read_table("//tmp/tm_client_test_table", format="yamr", raw=True).read() == "a\tb\nc\td\ne\tf\n"

    # Abort/restart
    task_id = client.add_task("titan", "tmp/yt/tm_client_test_table", "hahn", "//tmp/tm_client_test_table")
    client.abort_task(task_id)
    time.sleep(0.5)
    assert client.get_task_info(task_id)["state"] == "aborted"
    client.restart_task(task_id)
    assert client.get_task_info(task_id)["state"] != "aborted"
    _wait_task(task_id, client)

def test_copy_no_retries(backend_url):
    client = TransferManager(url=backend_url, enable_retries=False)

    hahn_client = Yt(proxy="hahn")
    table = hahn_client.create_temp_table()
    client.add_task("hahn", table, "banach", "//tmp/tm_client_test_table", sync=True)

def test_copy_dir(backend_url):
    client = TransferManager(url=backend_url, enable_retries=False)

    hahn_client = Yt(proxy="hahn")
    hahn_client.mkdir("//tmp/test_dir_tm", recursive=True)
    for i in xrange(10):
        hahn_client.create("table", "//tmp/test_dir_tm/" + str(i), ignore_existing=True)

    banach_client = Yt(proxy="banach")
    banach_client.remove("//tmp/test_dir_tm", recursive=True, force=True)

    client.add_tasks("hahn", "//tmp/test_dir_tm", "banach", "//tmp/test_dir_tm", sync=True, running_tasks_limit=2)

    assert 10 == len(banach_client.list("//tmp/test_dir_tm"))

def test_early_task_skipping(backend_url):
    client = TransferManager(url=backend_url)

    hahn_client = Yt(proxy="hahn")
    # NOTE: Creating intermediate dir here to avoid long listing //tmp directory in match.
    hahn_client.mkdir("//tmp/test_dir_tm_early_task_skipping", recursive=True)
    hahn_client.create("table", "//tmp/test_dir_tm_early_task_skipping/tm_client_test_table", ignore_existing=True)
    banach_client = Yt(proxy="banach")
    banach_client.create("table", "//tmp/tm_client_test_table", ignore_existing=True)

    task_ids = client.add_tasks(
        "hahn",
        "//tmp/test_dir_tm_early_task_skipping/tm_client_test_table",
        "banach",
        "//tmp/tm_client_test_table",
        enable_early_skip_if_destination_exists=True,
        params={"skip_if_destination_exists": True},
        sync=True)

    assert len(task_ids) == 0
