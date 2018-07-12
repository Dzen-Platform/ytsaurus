import pytest

from yp.client import YpResponseError
from yt.yson import YsonEntity, YsonUint64

@pytest.mark.usefixtures("yp_env")
class TestPods(object):
    def test_pod_set_required_on_create(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YpResponseError): yp_client.create_object(object_type="pod")

    def test_get_pod(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        pod_id = yp_client.create_object("pod", attributes={"meta": {"pod_set_id": pod_set_id}})
        result = yp_client.get_object("pod", pod_id, selectors=["/status/agent/state", "/meta/id", "/meta/pod_set_id"])
        assert result[0] == "unknown"
        assert result[1] == pod_id
        assert result[2] == pod_set_id

    def test_parent_pod_set_must_exist(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YpResponseError): yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": "nonexisting_pod_set_id"}})

    def test_pod_create_destroy(self, yp_env):
        yp_client = yp_env.yp_client
        yt_client = yp_env.yt_client

        def get_counts():
            return (len(list(yt_client.select_rows("* from [//yp/db/pod_sets] where is_null([meta.removal_time])"))),
                    len(list(yt_client.select_rows("* from [//yp/db/pods] where is_null([meta.removal_time])"))),
                    len(list(yt_client.select_rows("* from [//yp/db/parents]"))))

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_attributes = {"meta": {"pod_set_id": pod_set_id}}
        pod_ids = [yp_client.create_object(object_type="pod", attributes=pod_attributes)
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
        with pytest.raises(YpResponseError):
            yp_client.create_object("pod", attributes={
                    "meta": {
                        "pod_set_id": pod_set_id
                    },
                    "spec": {
                        "resource_requests": {
                            "vcpu_guarantee": 100,
                            "memory_limit": 2000
                        },
                        "node_id": node_id
                    }
                })

    def test_pod_assignment_cpu_memory_success(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, cpu_capacity=100, memory_capacity=2000)
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
                    "resource_requests": {
                        "vcpu_guarantee": 100,
                        "memory_limit": 2000
                    },
                    "node_id": node_id
                }
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
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        
        assert \
          sorted([hdd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_hdd_ssd_success(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
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
        
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

        assert \
          sorted([hdd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

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

        assert \
          sorted([hdd_resource_id, ssd_resource_id]) == \
          sorted([x["resource_id"] for x in yp_client.get_object("pod", pod_id, selectors=["/status/scheduled_resource_allocations"])[0]])

    def test_pod_assignment_exclusive_hdd_failure1(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })
        yp_client.select_objects("resource", selectors=["/spec", "/status"])
        with pytest.raises(YpResponseError): yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

    def test_pod_assignment_exclusive_hdd_failure2(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000, ssd_capacity=2000)
        with pytest.raises(YpResponseError): yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

    def test_pod_assignment_disk_volume_policy_check(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        
        def try_create_pod():
            yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

        yp_client.update_object("resource", hdd_resource_id, set_updates=[{"path": "/spec/disk/supported_policies", "value": ["exclusive"]}])
        with pytest.raises(YpResponseError): try_create_pod()
        yp_client.update_object("resource", hdd_resource_id, set_updates=[{"path": "/spec/disk/supported_policies", "value": ["quota"]}])
        try_create_pod()

    def test_disk_volume_request_ids(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        with pytest.raises(YpResponseError):
            yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
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
                }
            })

    def test_disk_volume_request_policy_required(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        with pytest.raises(YpResponseError):
            yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
                    "disk_volume_requests": [
                      {
                        "id": "a",
                        "storage_class": "hdd",
                      }
                    ]
                }
            })

    def test_disk_volume_request_update(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        
        pod_id = yp_client.create_object("pod", attributes={
            "meta": {
                "pod_set_id": pod_set_id
            },
            "spec": {
                "disk_volume_requests": [
                  {
                    "id": "hdd1",
                    "storage_class": "hdd",
                    "quota_policy": {"capacity": 500}
                  }
                ],
                "node_id": node_id
            }
        })

        allocations1 = yp_client.get_object("resource", hdd_resource_id, selectors=["/status/scheduled_allocations"])[0]

        yp_client.update_object("pod", pod_id, set_updates=[
            {
                "path": "/spec/disk_volume_requests/end",
                "value": {
                    "id": "hdd2",
                    "storage_class": "hdd",
                    "quota_policy": {"capacity": 100}
                }
            }])

        allocations2 = yp_client.get_object("resource", hdd_resource_id, selectors=["/status/scheduled_allocations"])[0]

        with pytest.raises(YpResponseError):
            yp_client.update_object("pod", pod_id, set_updates=[
                {
                    "path": "/spec/disk_volume_requests/0/quota_policy/capacity",
                    "value": YsonUint64(555)
                }])

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

        assert allocations1[0]["disk"] == {"exclusive": False, "volume_id": volume_id1, "capacity": YsonUint64(500)}
        
        assert allocations2[0]["disk"] == {"exclusive": False, "volume_id": volume_id1, "capacity": YsonUint64(500)}
        assert allocations2[1]["disk"] == {"exclusive": False, "volume_id": volume_id2, "capacity": YsonUint64(100)}

        assert allocations3[0]["disk"] == {"exclusive": False, "volume_id": volume_id3, "capacity": YsonUint64(555)}
        assert allocations3[1]["disk"] == {"exclusive": False, "volume_id": volume_id4, "capacity": YsonUint64(100)}

    def test_disk_allocation_in_pod_status(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client, hdd_capacity=1000)
        hdd_resource_id = yp_client.select_objects("resource", filter='[/meta/kind] = "disk" and [/spec/disk/storage_class] = "hdd"', selectors=["/meta/id"])[0][0]
        
        pod_id = yp_client.create_object("pod", attributes={
            "meta": {
                "pod_set_id": pod_set_id
            },
            "spec": {
                "disk_volume_requests": [
                  {
                    "id": "some_id",
                    "labels": {"some_key": "some_value"},
                    "storage_class": "hdd",
                    "quota_policy": {"capacity": 500}
                  }
                ],
                "node_id": node_id
            }
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
            yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                },
                "spec": {
                    "disk_volume_requests": [
                      {
                        "id": "some_id",
                        "storage_class": "hdd",
                        "quota_policy": {"capacity": 1}
                      }
                    ],
                    "node_id": node_id
                }
            })

        for i in xrange(5):
            create_pod()
        with pytest.raises(YpResponseError): create_pod()

    def test_pod_fqdns(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        node_id = self._create_node(yp_client)
        pod_id = yp_client.create_object("pod", attributes={
                "meta": {
                    "pod_set_id": pod_set_id
                }})

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
        pod_id = yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": pod_set_id}})
        
        ts1 = yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

        tx_id = yp_client.start_transaction()
        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/node_filter", "value": "0 = 1"}], transaction_id=tx_id)
        ts2 = yp_client.commit_transaction(tx_id)["commit_timestamp"]

        assert ts1 < ts2
        assert ts2 == yp_client.get_object("pod", pod_id, selectors=["/status/master_spec_timestamp"])[0]

    def test_iss_spec(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": pod_set_id}})

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
        pod_id = yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": pod_set_id}})

        assert yp_client.get_object("pod", pod_id, selectors=["/status/agent/iss_payload", "/status/agent/iss"]) == ["", {}]

    def test_spec_updates(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": pod_set_id}})

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/resource_requests", "value": {"vcpu_limit": 100}}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 100}
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests/vcpu_limit"])[0] == 100
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests"])[0] == [{"vcpu_limit": 100}]
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests/vcpu_limit"])[0] == [100]
        assert yp_client.select_objects("pod", selectors=["/spec"])[0][0]["resource_requests"] == {"vcpu_limit": 100}

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/resource_requests/vcpu_limit", "value": YsonUint64(200)}])
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 200}
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests/vcpu_limit"])[0] == 200
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests"])[0] == [{"vcpu_limit": 200}]
        assert yp_client.select_objects("pod", selectors=["/spec/resource_requests/vcpu_limit"])[0] == [200]
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/resource_requests"])[0] == {"vcpu_limit": 200}
        assert yp_client.select_objects("pod", selectors=["/spec"])[0][0]["resource_requests"] == {"vcpu_limit": 200}

    def test_incorrect_virtual_service_tunnel(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object(object_type="pod_set")
        with pytest.raises(YpResponseError) as create_error:
            yp_client.create_object(object_type="pod", attributes={
                "meta": {"pod_set_id": pod_set_id},
                "spec": {
                    "virtual_service_tunnel": {
                        "virtual_service_id": "incorrect_id",
                    },
                },
            })
        assert create_error.value.is_missing_object_id()

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
            with pytest.raises(YpResponseError) as create_error:
                yp_client.create_object(object_type="pod", attributes={
                    "meta": {"pod_set_id": pod_set_id},
                    "spec": {
                        "host_devices": [
                            incorrect_device
                        ],
                    },
                })
            assert "Host device \"{}\" cannot be configured".format(incorrect_device["path"]) in str(create_error.value)

        for correct_device in correct_host_devices:
            pod_id = yp_client.create_object(object_type="pod", attributes={
                "meta": {"pod_set_id": pod_set_id},
                "spec": {
                    "host_devices": [
                        correct_device
                    ],
                },
            })

            pod_spec = yp_client.get_object("pod", pod_id, selectors=["/spec"])[0]
            assert pod_spec["host_devices"][0]["path"] == correct_device["path"]
            assert pod_spec["host_devices"][0]["mode"] == correct_device["mode"]

    def test_virtual_service_tunnel(self, yp_env):
        yp_client = yp_env.yp_client

        virtual_service_id = yp_client.create_object(
            object_type="virtual_service",
            attributes={
                "spec": {
                    "ip4_addresses": ["100.100.100.100", "2.2.2.2"],
                    "ip6_addresses": ["1:1:1:1", "2:2:2:2", "3:3:3:3"],
                }
            })

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={
            "meta": {"pod_set_id": pod_set_id},
            "spec": {
                "virtual_service_tunnel": {
                    "virtual_service_id": virtual_service_id,
                    "ip6_mtu": 42,
                    "ip4_mtu": 36,
                }
            }
        })

        assert yp_client.get_object("pod", pod_id, selectors=["/spec/virtual_service_tunnel/virtual_service_id"])[0] == virtual_service_id
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/virtual_service_tunnel/ip6_mtu"])[0] == 42
        assert yp_client.get_object("pod", pod_id, selectors=["/spec/virtual_service_tunnel/ip4_mtu"])[0] == 36

        addresses = yp_client.get_object("pod", pod_id, selectors=["/status/virtual_service"])[0]
        assert addresses["ip4_addresses"][0] == "100.100.100.100"
        assert addresses["ip4_addresses"][1] == "2.2.2.2"
        assert addresses["ip6_addresses"][0] == "1:1:1:1"
        assert addresses["ip6_addresses"][1] == "2:2:2:2"
        assert addresses["ip6_addresses"][2] == "3:3:3:3"

    def test_update_virtual_service_tunnel(self, yp_env):
        yp_client = yp_env.yp_client

        specs = [
            { "ip4_addresses": ["1.2.3.4"] },
            { "ip6_addresses": ["1:2:3:4"] },
            { "ip4_addresses": ["1.2.3.4"], "ip6_addresses": ["1:2:3:4"] },
            { }
        ]

        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={"meta": {"pod_set_id": pod_set_id}})

        for spec in specs:
            vs_id = yp_client.create_object(object_type="virtual_service", attributes={"spec": spec})

            update = {
                "path": "/spec/virtual_service_tunnel",
                "value": {"virtual_service_id": vs_id}
            }

            def check_vs_status():
                addresses = yp_client.get_object("pod", pod_id, selectors=["/status/virtual_service"])[0]
                ip4, ip6 = addresses.get("ip4_addresses", []), addresses.get("ip6_addresses", [])
                spec_ip4, spec_ip6 = spec.get("ip4_addresses", []), spec.get("ip6_addresses", [])

                assert ip4 == spec_ip4
                assert ip6 == spec_ip6

            yp_client.update_object("pod", pod_id, set_updates=[update])
            check_vs_status()
            yp_client.update_object("pod", pod_id, set_updates=[update])
            check_vs_status()

            yp_client.update_object("pod", pod_id, remove_updates=[{"path": "/spec/virtual_service_tunnel"}])
            assert "virtual_service" not in yp_client.get_object("pod", pod_id, selectors=["/status"])[0]

    def test_host_devices(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = self._create_node(yp_client)
        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={
            "meta": {"pod_set_id": pod_set_id},
            "spec": {"node_id": node_id}
        })

        for x in ["/dev/kvm",
                  "/dev/net/tun"]:
            yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/host_devices", "value": [{"path": x, "mode": "rw"}]}])

        for x in ["/dev/xyz"]:
            with pytest.raises(YpResponseError):
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/host_devices", "value": [{"path": x, "mode": "rw"}]}])

    def test_sysctl_properties(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = self._create_node(yp_client)
        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(object_type="pod", attributes={
            "meta": {"pod_set_id": pod_set_id},
            "spec": {"node_id": node_id}
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
            with pytest.raises(YpResponseError):
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/sysctl_properties", "value": [{"name": x, "value": "0"}]}])

        for v in ["value; injection",
                  ";",
                  "; injection",
                  "injection;",
                  "   ;"]:
            with pytest.raises(YpResponseError) as create_error:
                yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/sysctl_properties", "value": [{"name": "net.core.somaxconn", "value": v}]}])
            assert "\";\" symbol is not allowed" in str(create_error.value)

    def test_default_antiaffinity_constraints_yp_365(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set")
        assert yp_client.get_object("pod_set", pod_set_id, selectors=["/spec/antiaffinity_constraints"]) == [[]]