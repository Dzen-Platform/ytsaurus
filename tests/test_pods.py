from .conftest import ZERO_RESOURCE_REQUESTS

from yp.common import YtResponseError, YpNoSuchObjectError

from yt.wrapper.errors import YtTabletTransactionLockConflict
from yt.wrapper.retries import run_with_retries

from yt.yson import YsonEntity, YsonUint64
import yt.common

from yt.packages.six.moves import xrange


import pytest


@pytest.mark.usefixtures("yp_env")
class TestPods(object):
    def _create_pod_with_boilerplate(self, yp_client, pod_set_id, spec):
        merged_spec = yt.common.update(
            {
                "resource_requests": ZERO_RESOURCE_REQUESTS
            },
            spec)
        return yp_client.create_object("pod", attributes={
            "meta": {
                "pod_set_id": pod_set_id
            },
            "spec": merged_spec
        })

    def _create_pod_with_boilerplate_and_retries(self, yp_client, pod_set_id, spec):
        merged_spec = yt.common.update(
            {
                "resource_requests": ZERO_RESOURCE_REQUESTS
            },
            spec)

        class DummyPodId(object):
            def __init__(self):
                self.pod_id = None

            def try_create(self, yp_client, pod_set_id, merged_spec):
                self.pod_id = yp_client.create_object("pod", attributes={
                    "meta": {
                        "pod_set_id": pod_set_id
                    },
                    "spec": merged_spec
                })

        pod_id = DummyPodId()
        # Exceptions may happen if some creation can force resources or node_id
        run_with_retries(lambda: pod_id.try_create(yp_client, pod_set_id, merged_spec), exceptions=(YtTabletTransactionLockConflict,))
        return pod_id.pod_id

    def test_pod_set_required_on_create(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YtResponseError):
            yp_client.create_object(object_type="pod")

    def test_get_pod(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})
        result = yp_client.get_object("pod", pod_id, selectors=["/status/agent/state", "/meta/id", "/meta/pod_set_id"])
        assert result[0] == "unknown"
        assert result[1] == pod_id
        assert result[2] == pod_set_id

    def test_get_pods(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id1 = yp_client.create_object("pod_set")
        pod_set_id2 = yp_client.create_object("pod_set")
        pod_set_id3 = yp_client.create_object("pod_set")

        pod_id11 = self._create_pod_with_boilerplate(yp_client, pod_set_id1, {})
        pod_id12 = self._create_pod_with_boilerplate(yp_client, pod_set_id1, {})
        pod_id13 = self._create_pod_with_boilerplate(yp_client, pod_set_id1, {})
        pod_id21 = self._create_pod_with_boilerplate(yp_client, pod_set_id2, {})
        pod_id22 = self._create_pod_with_boilerplate(yp_client, pod_set_id2, {})
        pod_id31 = self._create_pod_with_boilerplate(yp_client, pod_set_id3, {})

        result = yp_client.get_objects("pod", [pod_id11, pod_id12, pod_id31, pod_id21, pod_id22, pod_id13], selectors=["/meta/id", "/meta/pod_set_id"])
        assert len(result) == 6
        assert result[0][1] == pod_set_id1 and result[0][0] == pod_id11
        assert result[1][1] == pod_set_id1 and result[1][0] == pod_id12
        assert result[2][1] == pod_set_id3 and result[2][0] == pod_id31
        assert result[3][1] == pod_set_id2 and result[3][0] == pod_id21
        assert result[4][1] == pod_set_id2 and result[4][0] == pod_id22
        assert result[5][1] == pod_set_id1 and result[5][0] == pod_id13

        partial_result = yp_client.get_objects("pod", [pod_id31, pod_id12], selectors=["/meta/id", "/meta/pod_set_id"])
        assert len(partial_result) == 2
        assert partial_result[0][1] == pod_set_id3 and partial_result[0][0] == pod_id31
        assert partial_result[1][1] == pod_set_id1 and partial_result[1][0] == pod_id12

        yp_client.remove_object("pod", pod_id31)

        with pytest.raises(YtResponseError):
            yp_client.get_objects("pod", [pod_id31], selectors=["/meta/id"])

        with pytest.raises(YtResponseError):
            yp_client.get_objects("pod", [pod_id11, pod_id12, pod_id31, pod_id22], selectors=["/meta/id"])

        dups_result = yp_client.get_objects("pod", [pod_id11, pod_id11], selectors=["/meta/id"])
        assert dups_result[0][0] == pod_id11
        assert dups_result[1][0] == pod_id11

    def test_parent_pod_set_must_exist(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YpNoSuchObjectError):
            self._create_pod_with_boilerplate(yp_client, "nonexisting_pod_set_id", {})

    def test_pod_create_destroy(self, yp_env):
        yp_client = yp_env.yp_client
        yt_client = yp_env.yt_client

        def get_counts():
            return (len(list(yt_client.select_rows("* from [//yp/db/pod_sets] where is_null([meta.removal_time])"))),
                    len(list(yt_client.select_rows("* from [//yp/db/pods] where is_null([meta.removal_time])"))),
                    len(list(yt_client.select_rows("* from [//yp/db/parents]"))))

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_attributes = {"meta": {"pod_set_id": pod_set_id}}
        pod_ids = [self._create_pod_with_boilerplate(yp_client, pod_set_id, {})
                   for i in xrange(10)]

        assert get_counts() == (1, 10, 10)

        yp_client.remove_object("pod", pod_ids[0])

        assert get_counts() == (1, 9, 9)

        yp_client.remove_object("pod_set", pod_set_id)

        assert get_counts() == (0, 0, 0)

    def test_pod_set_empty_selectors(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        assert yp_client.get_object("pod_set", pod_set_id, selectors=[]) == []

    def _create_node(self, yp_client, cpu_capacity=0, memory_capacity=0, hdd_capacity=0, hdd_volume_slots = 10, ssd_capacity=0, ssd_volume_slots=10):
        node_id = "test.yandex.net"
        assert yp_client.create_object("node", attributes={
                "meta": {
                    "id": node_id
                }
            }) == node_id
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "cpu": {"total_capacity": cpu_capacity}
                }
            })
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "memory": {"total_capacity": memory_capacity}
                }
            })
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "disk": {
                      "total_capacity": hdd_capacity,
                      "total_volume_slots": hdd_volume_slots,
                      "supported_policies": ["quota", "exclusive"],
                      "storage_class": "hdd",
                      "device": "/dev/hdd"
                    }
                }
            })
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "disk": {
                      "total_capacity": ssd_capacity,
                      "total_volume_slots": ssd_volume_slots,
                      "supported_policies": ["quota", "exclusive"],
                      "storage_class": "ssd",
                      "device": "/dev/ssd"
                    }
                }
            })
        return node_id

    def test_pod_assignment_cpu_memory_failure(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client)
        with pytest.raises(YtResponseError):
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "resource_requests": {
                        "vcpu_guarantee": 100,
                        "memory_limit": 2000
                    },
                    "node_id": node_id
                })

    def test_pod_assignment_cpu_memory_success(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, cpu_capacity=100, memory_capacity=2000)
        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "resource_requests": {
                    "vcpu_guarantee": 100,
                    "memory_limit": 2000
                },
                "node_id": node_id
            })

        cpu_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "cpu"', selectors=["/meta/id"])[0][0]
        memory_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "memory"', selectors=["/meta/id"])[0][0]

        assert \
          sorted([cpu_resource_id, memory_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_hdd_success(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)
        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd",
                    "storage_class": "hdd",
                    "quota_policy": {
                      "capacity": 500
                    }
                  }
                ],
                "node_id": node_id
            })

        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]

        assert \
          sorted([hdd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_hdd_ssd_success(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)
        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd",
                    "storage_class": "hdd",
                    "quota_policy": {
                      "capacity": 500
                    }
                  },
                  {
                    "id": "ssd",
                    "storage_class": "ssd",
                    "quota_policy": {
                      "capacity": 600
                    }
                  }
                ],
                "node_id": node_id
            })

        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        ssd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "ssd"', selectors=["/meta/id"])[0][0]

        assert \
          sorted([hdd_resource_id, ssd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_exclusive_hdd_with_update(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)

        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        ssd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "ssd"', selectors=["/meta/id"])[0][0]

        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd",
                    "storage_class": "hdd",
                    "exclusive_policy": {
                      "min_capacity": 500
                    }
                  }
                ],
                "node_id": node_id
            })

        assert \
          sorted([hdd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

        def try_update(yp_client, pod_id):
            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/disk_volume_requests/end",
                    "value": {
                        "id": "ssd",
                        "storage_class": "ssd",
                        "quota_policy": {
                          "capacity": 600
                        }
                  }
                }])

        run_with_retries(lambda: try_update(yp_client, pod_id))

        assert \
          sorted([hdd_resource_id, ssd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_exclusive_hdd_failure1(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)

        self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd",
                    "storage_class": "hdd",
                    "exclusive_policy": {
                      "min_capacity": 500
                    }
                  }
                ],
                "node_id": node_id
            })

        yp_client.select_objects("resource", selectors=["/spec", "/status"])
        with pytest.raises(YtResponseError):
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd",
                    "storage_class": "hdd",
                    "exclusive_policy": {
                      "min_capacity": 500
                    }
                  }
                ],
                "node_id": node_id
            })

    def test_pod_assignment_exclusive_hdd_failure2(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)
        with pytest.raises(YtResponseError):
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd1",
                    "storage_class": "hdd",
                    "exclusive_policy": {
                      "min_capacity": 500
                    }
                  },
                  {
                    "id": "hdd2",
                    "storage_class": "hdd",
                    "exclusive_policy": {
                      "min_capacity": 500
                    }
                  }
                ],
                "node_id": node_id
            })

    def test_pod_assignment_disk_volume_policy_check(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]

        def try_create_pod():
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "disk_volume_requests": [
                      {
                        "id": "hdd",
                        "storage_class": "hdd",
                        "quota_policy": {
                          "capacity": 500
                        }
                      }
                    ],
                    "node_id": node_id
                })

        yp_client.update_object("resource", hdd_resource_id, set_updates=[{"path": "/spec/disk/supported_policies", "value": ["exclusive"]}])
        with pytest.raises(YtResponseError):
            try_create_pod()
        yp_client.update_object("resource", hdd_resource_id, set_updates=[{"path": "/spec/disk/supported_policies", "value": ["quota"]}])
        run_with_retries(try_create_pod, exceptions=(YtTabletTransactionLockConflict,))

    def test_disk_volume_request_ids(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        with pytest.raises(YtResponseError):
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "disk_volume_requests": [
                      {
                        "id": "a",
                        "storage_class": "hdd",
                        "quota_policy": { "capacity": 500 }
                      },
                      {
                        "id": "a",
                        "storage_class": "hdd",
                        "quota_policy": { "capacity": 100 }
                      },
                    ]
                })

    def test_disk_volume_request_policy_required(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        with pytest.raises(YtResponseError):
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "disk_volume_requests": [
                      {
                        "id": "a",
                        "storage_class": "hdd",
                      }
                    ]
                })

    def test_disk_volume_request_update(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]

        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "hdd1",
                    "storage_class": "hdd",
                    "quota_policy": {"capacity": 500}
                  }
                ],
                "node_id": node_id
            })

        allocations1 = yp_client.get_object("resource", hdd_resource_id, selectors=["/status/scheduled_allocations"])[0]

        def try_update(yp_client, pod_id):
            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/disk_volume_requests/end",
                    "value": {
                        "id": "hdd2",
                        "storage_class": "hdd",
                        "quota_policy": {"capacity": 100}
                    }
                }])

        run_with_retries(lambda: try_update(yp_client, pod_id), exceptions=(YtTabletTransactionLockConflict,))

        allocations2 = yp_client.get_object("resource", hdd_resource_id, selectors=["/status/scheduled_allocations"])[0]

        with pytest.raises(YtResponseError):
            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/disk_volume_requests/0/quota_policy/capacity",
                    "value": YsonUint64(555)
                }])

        def try_update_2(yp_client, pod_id, node_id):
            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/node_id",
                    "value": ""
                }])

            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/disk_volume_requests/0/quota_policy/capacity",
                    "value": YsonUint64(555)
                }])

            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/node_id",
                    "value": node_id
                }])

        run_with_retries(lambda: try_update_2(yp_client, pod_id, node_id), exceptions=(YtTabletTransactionLockConflict,))

        allocations3 = yp_client.get_object("resource", hdd_resource_id, selectors=["/status/scheduled_allocations"])[0]

        assert len(allocations1) == 1
        assert len(allocations2) == 2
        assert len(allocations3) == 2

        for a in [allocations1, allocations2, allocations3]:
            for b in a:
                assert b["pod_id"] == pod_id

        volume_id1 = allocations1[0]["disk"]["volume_id"]
        volume_id2 = allocations2[1]["disk"]["volume_id"]
        volume_id3 = allocations3[0]["disk"]["volume_id"]
        volume_id4 = allocations3[1]["disk"]["volume_id"]

        assert allocations1[0]["disk"] == {
            "exclusive": False,
            "volume_id": volume_id1,
            "capacity": YsonUint64(500),
            "bandwidth": YsonUint64(0),
        }

        assert allocations2[0]["disk"] == {
            "exclusive": False,
            "volume_id": volume_id1,
            "capacity": YsonUint64(500),
            "bandwidth": YsonUint64(0),
        }
        assert allocations2[1]["disk"] == {
            "exclusive": False,
            "volume_id": volume_id2,
            "capacity": YsonUint64(100),
            "bandwidth": YsonUint64(0),
        }

        assert allocations3[0]["disk"] == {
            "exclusive": False,
            "volume_id": volume_id3,
            "capacity": YsonUint64(555),
            "bandwidth": YsonUint64(0),
        }
        assert allocations3[1]["disk"] == {
            "exclusive": False,
            "volume_id": volume_id4,
            "capacity": YsonUint64(100),
            "bandwidth": YsonUint64(0),
        }

    def test_disk_allocation_in_pod_status(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]

        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "disk_volume_requests": [
                  {
                    "id": "some_id",
                    "labels": {"some_key": "some_value"},
                    "storage_class": "hdd",
                    "quota_policy": {"capacity": 500}
                  }
                ],
                "node_id": node_id
            })

        allocations = yp_client.get_object("pod", pod_id, selectors=["/status/disk_volume_allocations"])[0]
        assert len(allocations) == 1
        allocation = allocations[0]
        assert allocation["id"] == "some_id"
        assert allocation["labels"] == {"some_key": "some_value"}
        assert allocation["capacity"] == YsonUint64(500)
        assert allocation["resource_id"] == hdd_resource_id
        assert allocation["volume_id"] != ""
        assert allocation["device"] == yp_client.get_object("resource", hdd_resource_id, selectors=["/spec/disk/device"])[0]

    def test_disk_resource_volume_slot_limit(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, hdd_volume_slots=5)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]

        def create_pod():
            self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "disk_volume_requests": [
                      {
                        "id": "some_id",
                        "storage_class": "hdd",
                        "quota_policy": {"capacity": 1}
                      }
                    ],
                    "node_id": node_id
                })

        for i in xrange(5):
            run_with_retries(create_pod, exceptions=(YtTabletTransactionLockConflict,))

        with pytest.raises(YtResponseError):
            create_pod()

    def test_pod_fqdns(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client)
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})

        assert yp_client.get_object("pod", pod_id, selectors=["/status/dns/persistent_fqdn", "/status/dns/transient_fqdn"]) == \
               [pod_id + ".test.yp-c.yandex.net", YsonEntity()]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/node_id", "value": node_id}])

        assert yp_client.get_object("pod", pod_id, selectors=["/status/dns/persistent_fqdn", "/status/dns/transient_fqdn", "/status/generation_number"]) == \
               [pod_id + ".test.yp-c.yandex.net", "test-1." + pod_id + ".test.yp-c.yandex.net", 1]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/node_id", "value": ""}])

        assert yp_client.get_object("pod", pod_id, selectors=["/status/dns/persistent_fqdn", "/status/dns/transient_fqdn", "/status/generation_number"]) == \
               [pod_id + ".test.yp-c.yandex.net", YsonEntity(), 1]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/node_id", "value": node_id}])

        assert yp_client.get_object("pod", pod_id, selectors=["/status/dns/persistent_fqdn", "/status/dns/transient_fqdn", "/status/generation_number"]) == \
               [pod_id + ".test.yp-c.yandex.net", "test-2." + pod_id + ".test.yp-c.yandex.net", 2]

    def test_master_spec_timestamp(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})

        ts1 = yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

        tx_id = yp_client.start_transaction()
        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/node_filter", "value": "0 = 1"}], transaction_id=tx_id)
        ts2 = yp_client.commit_transaction(tx_id)["commit_timestamp"]

        assert ts1 < ts2
        assert ts2 == yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

    def test_iss_spec(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})

        iss_spec = {
            "instances": [{
                "id": {
                    "slot": {
                        "service": "some-service",
                        "host": "some-host"
                    },
                    "configuration": {
                        "groupId": "some-groupId",
                    }
                }
            }]
        }

        assert yp_client.get_object("pod", pod_id, selectors=["/spec/iss"])[0] == {}
        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/iss", "value": iss_spec}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/iss"])[0] == iss_spec

        yp_client.get_object("pod", pod_id, selectors=["/spec/iss_payload"])[0]

        yp_client.update_object("pod",pod_id, set_updates=[{"path": "/spec/iss/instances/0/id/slot/service", "value": "another-service"}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/iss/instances/0/id/slot/service"])[0] == "another-service"

    def test_iss_status(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})

        assert yp_client.get_object("pod", pod_id, selectors=["/status/agent/iss_payload", "/status/agent/iss"]) == ["", {}]

    def test_spec_updates(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/resource_requests", "value": {"vcpu_limit": 100}}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 100}
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests/vcpu_limit"])[0] == 100
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests"])[0] == [{"vcpu_limit": 100}]
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests/vcpu_limit"])[0] == [100]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/resource_requests/vcpu_limit", "value": YsonUint64(200)}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 200}
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests/vcpu_limit"])[0] == 200
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests"])[0] == [{"vcpu_limit": 200}]
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests/vcpu_limit"])[0] == [200]
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 200}

    def test_host_device_constraints(self, yp_env):
        yp_client = yp_env.yp_client

        incorrect_host_devices = [
            { "path": "/dev/42", "mode": "r" },
            { "path": "/dev/kvm", "mode": "io" },
            { "path": "/dev/kvm", "mode": "rwt" },
            { "path": "/dev/net/tun", "mode": "rw; 42" },
        ]

        for symbol in map(chr, range(0, 256)):
            if symbol not in "rwm-":
                incorrect_host_devices.append({"path": "/dev/kvm", "mode": symbol})

        correct_host_devices = [
            { "path": "/dev/kvm", "mode": "r", },
            { "path": "/dev/net/tun", "mode": "rwm-" },
        ]

        for symbol in "rwm-":
            correct_host_devices.append({"path": "/dev/kvm", "mode": symbol})

        pod_set_id = yp_client.create_object(object_type="pod_set")

        for incorrect_device in incorrect_host_devices:
            with pytest.raises(YtResponseError) as create_error:
                self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                        "host_devices": [
                            incorrect_device
                        ]
                    })
            assert "Host device \"{}\" cannot be configured".format(incorrect_device["path"]) in str(create_error.value)

        for correct_device in correct_host_devices:
            pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                    "host_devices": [
                        correct_device
                    ]
                })

            pod_spec = yp_client.get_object("pod", pod_id, selectors=["/spec"])[0]
            assert pod_spec["host_devices"][0]["path"] == correct_device["path"]
            assert pod_spec["host_devices"][0]["mode"] == correct_device["mode"]

    def test_host_devices(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = self._create_node(yp_client)
        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "node_id": node_id
            })

        for x in ["/dev/kvm",
                  "/dev/net/tun"]:
            yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/host_devices", "value": [{"path": x, "mode": "rw"}]}])

        for x in ["/dev/xyz"]:
            with pytest.raises(YtResponseError):
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/host_devices", "value": [{"path": x, "mode": "rw"}]}])

    def test_sysctl_properties(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = self._create_node(yp_client)
        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = self._create_pod_with_boilerplate_and_retries(yp_client, pod_set_id, {
                "node_id": node_id
            })

        for x in ["net.core.somaxconn",
                  "net.unix.max_dgram_qlen",
                  "net.ipv4.icmp_echo_ignore_all",
                  "net.ipv4.icmp_echo_ignore_broadcasts",
                  "net.ipv4.icmp_ignore_bogus_error_responses",
                  "net.ipv4.icmp_errors_use_inbound_ifaddr",
                  "net.ipv4.icmp_ratelimit",
                  "net.ipv4.icmp_ratemask",
                  "net.ipv4.ping_group_range",
                  "net.ipv4.tcp_ecn",
                  "net.ipv4.tcp_ecn_fallback",
                  "net.ipv4.ip_dynaddr",
                  "net.ipv4.ip_early_demux",
                  "net.ipv4.ip_default_ttl",
                  "net.ipv4.ip_local_port_range",
                  "net.ipv4.ip_local_reserved_ports",
                  "net.ipv4.ip_no_pmtu_disc",
                  "net.ipv4.ip_forward_use_pmtu",
                  "net.ipv4.ip_nonlocal_bind",
                  "net.ipv4.tcp_mtu_probing",
                  "net.ipv4.tcp_base_mss",
                  "net.ipv4.tcp_probe_threshold",
                  "net.ipv4.tcp_probe_interval",
                  "net.ipv4.tcp_keepalive_time",
                  "net.ipv4.tcp_keepalive_probes",
                  "net.ipv4.tcp_keepalive_intvl",
                  "net.ipv4.tcp_syn_retries",
                  "net.ipv4.tcp_synack_retries",
                  "net.ipv4.tcp_syncookies",
                  "net.ipv4.tcp_reordering",
                  "net.ipv4.tcp_retries1",
                  "net.ipv4.tcp_retries2",
                  "net.ipv4.tcp_orphan_retries",
                  "net.ipv4.tcp_fin_timeout",
                  "net.ipv4.tcp_notsent_lowat",
                  "net.ipv4.tcp_tw_reuse",
                  "net.ipv6.bindv6only",
                  "net.ipv6.ip_nonlocal_bind",
                  "net.ipv6.icmp.ratelimit",
                  "net.ipv6.route.gc_thresh",
                  "net.ipv6.route.max_size",
                  "net.ipv6.route.gc_min_interval",
                  "net.ipv6.route.gc_timeout",
                  "net.ipv6.route.gc_interval",
                  "net.ipv6.route.gc_elasticity",
                  "net.ipv6.route.mtu_expires",
                  "net.ipv6.route.min_adv_mss",
                  "net.ipv6.route.gc_min_interval_ms",
                  "net.ipv4.conf.blablabla",
                  "net.ipv6.conf.blablabla",
                  "net.ipv4.neigh.blablabla",
                  "net.ipv6.neigh.blablabla"]:
            yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/sysctl_properties", "value": [{"name": x, "value": "0"}]}])

        for x in ["someother.property",
                  "net.ipv4.blablabla",
                  "net.ipv4.neigh.default.blablabla"]:
            with pytest.raises(YtResponseError):
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/sysctl_properties", "value": [{"name": x, "value": "0"}]}])

        for v in ["value; injection",
                  ";",
                  "; injection",
                  "injection;",
                  "   ;"]:
            with pytest.raises(YtResponseError) as create_error:
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/sysctl_properties", "value": [{"name": "net.core.somaxconn", "value": v}]}])
            assert "\";\" symbol is not allowed" in str(create_error.value)

    def test_default_antiaffinity_constraints_yp_365(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        assert yp_client.get_object("pod_set", pod_set_id, selectors=["/spec/antiaffinity_constraints"]) == [[]]

    def test_default_pod_acl(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {})
        assert yp_client.get_object("pod", pod_id, selectors=["/meta/acl"])[0] == []

    def test_dynamic_properties(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                "dynamic_attributes": {
                    "labels": ["l1", "l2"],
                    "annotations": ["a1", "a2"]
                }
            })

        ts1 = yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/labels/l3", "value": 1}])
        assert ts1 == yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/annotations/a3", "value": 1}])
        assert ts1 == yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/labels/l1", "value": 2}])
        ts2 = yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]
        assert ts2 > ts1

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/annotations/a1", "value": "hello"}])
        ts3 = yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]
        assert ts3 > ts2

        assert yp_client.get_object("pod", pod_id, selectors=["/status/pod_dynamic_attributes"])[0] == \
            {
                "labels": {"l1": 2},
                "annotations": {"a1": "hello"}
            }

    def test_resource_cache(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        pod_id = self._create_pod_with_boilerplate(yp_client, pod_set_id, {
                "resource_cache": {
                    "spec": {
                        "layers": [
                            {
                                "revision": 10,
                                "layer": {
                                    "id": "my_layer",
                                    "url": "my_layer_url",
                                    "checksum": "my_layer_checksum"
                                }
                            }
                        ],
                        "static_resources": [
                            {
                                "revision": 11,
                                "resource": {
                                    "id": "my_static_resource",
                                    "url": "my_static_resource_url",
                                    "verification": {
                                        "checksum": "my_static_resource_checksum",
                                        "check_period_ms": 20
                                    }
                                }
                            }
                        ]
                    }
                }
            })

        resource_cache_get = yp_client.get_object("pod", pod_id, selectors=["/spec/resource_cache"])[0]
        assert len(resource_cache_get["spec"]["layers"]) == 1
        assert resource_cache_get["spec"]["layers"][0]["revision"] == 10
        assert resource_cache_get["spec"]["layers"][0]["layer"]["id"] == "my_layer"
        assert resource_cache_get["spec"]["layers"][0]["layer"]["url"] == "my_layer_url"
        assert resource_cache_get["spec"]["layers"][0]["layer"]["checksum"] == "my_layer_checksum"
        assert len(resource_cache_get["spec"]["static_resources"]) == 1
        assert resource_cache_get["spec"]["static_resources"][0]["revision"] == 11
        assert resource_cache_get["spec"]["static_resources"][0]["resource"]["id"] == "my_static_resource"
        assert resource_cache_get["spec"]["static_resources"][0]["resource"]["url"] == "my_static_resource_url"
        assert resource_cache_get["spec"]["static_resources"][0]["resource"]["verification"]["checksum"] == "my_static_resource_checksum"
        assert resource_cache_get["spec"]["static_resources"][0]["resource"]["verification"]["check_period_ms"] == 20
