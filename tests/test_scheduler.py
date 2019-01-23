from .conftest import ZERO_RESOURCE_REQUESTS

from yp.local import set_account_infinite_resource_limits

from yp.common import YtResponseError, wait, WaitFailed

from yt.packages.six.moves import xrange

from yt.yson import YsonEntity, YsonUint64
import yt.common

import pytest

from collections import defaultdict
from functools import partial
import time


def _get_pod_scheduling_status(yp_client, pod_id):
    return yp_client.get_object("pod", pod_id, selectors=["/status/scheduling"])[0]

def _is_assigned_pod_scheduling_status(scheduling_status):
    return "error" not in scheduling_status and \
        scheduling_status.get("state", None) == "assigned" and \
        scheduling_status.get("node_id", "") != ""

def _is_error_pod_scheduling_status(scheduling_status):
    return "error" in scheduling_status and \
        scheduling_status.get("state", None) != "assigned" and \
        scheduling_status.get("node_id", None) is None

DEFAULT_ACCOUNT_ID = "tmp"

DEFAULT_POD_SET_SPEC = {
        "account_id": DEFAULT_ACCOUNT_ID,
        "node_segment_id": "default"
    }

def _create_pod_with_boilerplate(yp_client, pod_set_id, spec, pod_id=None, transaction_id=None):
    meta = {
        "pod_set_id": pod_set_id
    }
    if pod_id is not None:
        meta["id"] = pod_id
    merged_spec = yt.common.update(
        dict(resource_requests=ZERO_RESOURCE_REQUESTS),
        spec,
    )
    return yp_client.create_object("pod", attributes={
        "meta": meta,
        "spec": merged_spec,
    }, transaction_id=transaction_id)

def _create_nodes(
        yp_env,
        node_count,
        rack_count=1,
        hfsm_state="up",
        cpu_total_capacity=100,
        memory_total_capacity=1000000000,
        disk_total_capacity=100000000000,
        disk_total_volume_slots=10):
    yp_client = yp_env.yp_client

    node_ids = []
    for i in xrange(node_count):
        node_id = yp_client.create_object("node", attributes={
                "spec": {
                    "ip6_subnets": [
                        {"vlan_id": "backbone", "subnet": "1:2:3:4::/64"}
                    ]
                },
                "labels" : {
                    "topology": {
                        "node": "node-{}".format(i),
                        "rack": "rack-{}".format(i // (node_count // rack_count)),
                        "dc": "butovo"
                    }
                }
            })
        yp_client.update_hfsm_state(node_id, hfsm_state, "Test")
        node_ids.append(node_id)
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "cpu": {
                        "total_capacity": cpu_total_capacity,
                    }
                }
            })
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "memory": {
                        "total_capacity": memory_total_capacity,
                    }
                }
            })
        yp_client.create_object("resource", attributes={
                "meta": {
                    "node_id": node_id
                },
                "spec": {
                    "disk": {
                        "total_capacity": disk_total_capacity,
                        "total_volume_slots": disk_total_volume_slots,
                        "storage_class": "hdd",
                        "supported_policies": ["quota", "exclusive"],
                    }
                }
            })
    return node_ids

@pytest.mark.usefixtures("yp_env")
class TestScheduler(object):
    def _get_scheduled_allocations(self, yp_env, node_id, kind):
        yp_client = yp_env.yp_client
        results = yp_client.select_objects("resource", filter="[/meta/kind] = \"{}\" and [/meta/node_id] = \"{}\"".format(kind, node_id),
                                           selectors=["/status/scheduled_allocations"])
        assert len(results) == 1
        return results[0][0]

    def test_create_with_enabled_scheduling(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        node_id = yp_client.create_object("node")
        with pytest.raises(YtResponseError):
            _create_pod_with_boilerplate(yp_client, pod_set_id, {
                "enable_scheduling": True,
                "node_id": node_id
            })

    def test_force_assign(self, yp_env):
        yp_client = yp_env.yp_client

        node_ids = _create_nodes(yp_env, 1)
        node_id = node_ids[0]

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "enable_scheduling": False,
            "node_id": node_id
        })

        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"]) == ["assigned"]

        assert self._get_scheduled_allocations(yp_env, node_id, "cpu") == []
        assert self._get_scheduled_allocations(yp_env, node_id, "memory") == []

        yp_client.update_object("pod", pod_id,
            set_updates=[
                {"path": "/spec/node_id", "value": ""}
            ])

        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"]) == ["disabled"]

        yp_client.update_object("pod", pod_id,
            set_updates=[
                {"path": "/spec/node_id", "value": node_id}
            ])

        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"]) == ["assigned"]

    def test_enable_disable_scheduling(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "enable_scheduling": False,
        })

        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"]) == ["disabled"]

        node_ids = _create_nodes(yp_env, 10)

        with pytest.raises(YtResponseError):
            yp_client.update_object("pod", pod_id,
                set_updates=[
                    {"path": "/spec/enable_scheduling", "value": True},
                    {"path": "/spec/node_id", "value": node_ids[0]}
                ])

        yp_client.update_object("pod", pod_id,
            set_updates=[
                {"path": "/spec/enable_scheduling", "value": True}
            ])

        wait(lambda: yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

    def test_simple(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 100
            },
            "enable_scheduling": True
        })

        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"])[0] == "pending"

        _create_nodes(yp_env, 10)

        wait(lambda: yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

        node_id = yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0]

        allocations = self._get_scheduled_allocations(yp_env, node_id, "cpu")
        assert len(allocations) == 1
        assert "pod_uuid" in allocations[0] and allocations[0]["pod_uuid"]
        assert allocations[0]["pod_id"] == pod_id
        assert allocations[0]["cpu"] == {"capacity": 100}

        assert self._get_scheduled_allocations(yp_env, node_id, "memory") == []

        yp_client.update_object("pod", pod_id, set_updates=[{"path": "/spec/resource_requests/vcpu_guarantee", "value": YsonUint64(50)}])

        allocations = self._get_scheduled_allocations(yp_env, node_id, "cpu")
        assert len(allocations) == 1
        assert "pod_uuid" in allocations[0] and allocations[0]["pod_uuid"]
        assert allocations[0]["pod_id"] == pod_id
        assert allocations[0]["cpu"] == {"capacity": 50}

        assert self._get_scheduled_allocations(yp_env, node_id, "memory") == []

    def test_removed_allocations_conflict_transaction(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        first_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 1
            },
            "enable_scheduling": True
        }, pod_id="first")

        _create_nodes(yp_env, 1)

        wait(lambda: yp_client.get_object("pod", first_pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", first_pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

        transaction_id = yp_client.start_transaction()

        yp_client.remove_object("pod", first_pod_id, transaction_id=transaction_id)

        second_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 1
            },
            "enable_scheduling": True
        }, pod_id="second")

        wait(lambda: yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

        yp_client.commit_transaction(transaction_id)

        node_id = yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/node_id"])[0]

        wait(lambda: len(self._get_scheduled_allocations(yp_env, node_id, "cpu")) == 1)

    def test_removed_allocations_conflict_transaction_same_pod(self, yp_env):
        # TODO(danlark@) investigate why i can't create same pod in transaction
        return
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        first_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 1
            },
            "enable_scheduling": True
        }, pod_id="first")

        _create_nodes(yp_env, 1)

        wait(lambda: yp_client.get_object("pod", first_pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", first_pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

        transaction_id = yp_client.start_transaction()

        yp_client.remove_object("pod", first_pod_id, transaction_id=transaction_id)

        first_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 1
            },
            "enable_scheduling": True
        }, pod_id="first", transaction_id=transaction_id)

        second_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 1
            },
            "enable_scheduling": True
        }, pod_id="second")

        wait(lambda: yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/node_id"])[0] != "" and
                     yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/state"])[0] == "assigned")

        yp_client.commit_transaction(transaction_id)

        node_id = yp_client.get_object("pod", second_pod_id, selectors=["/status/scheduling/node_id"])[0]

        wait(lambda: len(self._get_scheduled_allocations(yp_env, node_id, "cpu")) == 1)

    def test_cpu_limit(self, yp_env):
        yp_client = yp_env.yp_client

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        for i in xrange(10):
            pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
                "resource_requests": {
                    "vcpu_guarantee": 30
                },
                "enable_scheduling": True
            })
        _create_nodes(yp_env, 1)

        wait(lambda: len(yp_client.select_objects("pod", filter="[/status/scheduling/state] = \"assigned\"", selectors=["/meta/id"])) > 0)
        assert len(yp_client.select_objects("pod", filter="[/status/scheduling/state] = \"assigned\"", selectors=["/meta/id"])) == 3

        node_id = yp_client.select_objects("node", selectors=["/meta/id"])[0][0]
        unassigned_pod_id = yp_client.select_objects("pod", filter="[/status/scheduling/state] != \"assigned\"", selectors=["/meta/id"])[0][0]
        with pytest.raises(YtResponseError):
            yp_client.update_object("pod", unassigned_pod_id, set_updates=[{"path": "/spec/node_id", "value": node_id}])

    def test_node_filter(self, yp_env):
        yp_client = yp_env.yp_client

        node_ids = _create_nodes(yp_env, 10)
        good_node_id = node_ids[5]
        yp_client.update_object("node", good_node_id, set_updates=[
            {"path": "/labels/status", "value": "good"}
        ])

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "resource_requests": {
                "vcpu_guarantee": 100
            },
            "enable_scheduling": True,
            "node_filter" : '[/labels/status] = "good"'
        })

        wait(lambda: yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0] == good_node_id)

    def test_malformed_node_filter(self, yp_env):
        yp_client = yp_env.yp_client

        _create_nodes(yp_env, 10)
        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "enable_scheduling": True,
            "node_filter" : '[/some/nonexisting] = "value"'
        })

        wait(lambda: yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0] == YsonEntity())

    def _wait_for_pod_assignment(self, yp_env):
        yp_client = yp_env.yp_client
        wait(lambda: all(x[0] == "assigned"
                         for x in yp_client.select_objects("pod", selectors=["/status/scheduling/state"])))

    def test_antiaffinity_per_node(self, yp_env):
        yp_client = yp_env.yp_client

        _create_nodes(yp_env, 10)
        pod_set_id = yp_client.create_object("pod_set", attributes={
                "spec": dict(DEFAULT_POD_SET_SPEC, **{
                    "antiaffinity_constraints": [
                        {"key": "node", "max_pods": 1}
                    ]
                })
            })
        for i in xrange(10):
            _create_pod_with_boilerplate(yp_client, pod_set_id, {
                "enable_scheduling": True,
            })

        self._wait_for_pod_assignment(yp_env)
        node_ids = set(x[0] for x in yp_client.select_objects("pod", selectors=["/status/scheduling/node_id"]))
        assert len(node_ids) == 10

    def test_antiaffinity_per_node_and_rack(self, yp_env):
        yp_client = yp_env.yp_client

        _create_nodes(yp_env, 10, 2)
        pod_set_id = yp_client.create_object("pod_set", attributes={
                "spec": dict(DEFAULT_POD_SET_SPEC, **{
                    "antiaffinity_constraints": [
                        {"key": "node", "max_pods": 1},
                        {"key": "rack", "max_pods": 3}
                    ],
                })
            })
        for i in xrange(6):
            _create_pod_with_boilerplate(yp_client, pod_set_id, {
                "enable_scheduling": True,
            })

        self._wait_for_pod_assignment(yp_env)
        node_ids = set(x[0] for x in yp_client.select_objects("pod", selectors=["/status/scheduling/node_id"]))
        assert len(node_ids) == 6

        rack_to_counter = defaultdict(int)
        for node_id in node_ids:
            rack = yp_client.get_object("node", node_id, selectors=["/labels/topology/rack"])[0]
            rack_to_counter[rack] += 1
        assert all(rack_to_counter[rack] == 3 for rack in rack_to_counter)

    def test_assign_to_up_nodes_only(self, yp_env):
        yp_client = yp_env.yp_client

        node_ids = _create_nodes(yp_env, 10, hfsm_state="down")

        up_node_id = node_ids[5]
        yp_client.update_hfsm_state(up_node_id, "up", "Test")

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        for _ in xrange(10):
            _create_pod_with_boilerplate(yp_client, pod_set_id, {
                "enable_scheduling": True,
            })

        self._wait_for_pod_assignment(yp_env)
        assert all(x[0] == up_node_id for x in yp_client.select_objects("pod", selectors=["/status/scheduling/node_id"]))

    def test_node_maintenance(self, yp_env):
        yp_client = yp_env.yp_client

        node_ids = _create_nodes(yp_env, 1)
        node_id = node_ids[0]

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_ids = []
        for _ in xrange(10):
            pod_ids.append(_create_pod_with_boilerplate(yp_client, pod_set_id, {
                "enable_scheduling": True
            }))

        self._wait_for_pod_assignment(yp_env)
        assert all(x[0] == node_id for x in yp_client.select_objects("pod", selectors=["/status/scheduling/node_id"]))
        assert all(x[0] == "none" for x in yp_client.select_objects("pod", selectors=["/status/eviction/state"]))

        yp_client.update_hfsm_state(node_id, "prepare_maintenance", "Test")
        assert yp_client.get_object("node", node_id, selectors=["/status/maintenance/state"])[0] == "requested"

        wait(lambda: all(x[0] == "requested" for x in yp_client.select_objects("pod", selectors=["/status/eviction/state"])))

        for pod_id in pod_ids:
            yp_client.acknowledge_pod_eviction(pod_id, "Test")

        wait(lambda: all(x[0] == YsonEntity() for x in yp_client.select_objects("pod", selectors=["/status/scheduling/node_id"])))
        assert all(x[0] == "none" for x in yp_client.select_objects("pod", selectors=["/status/eviction/state"]))

        wait(lambda: yp_client.get_object("node", node_id, selectors=["/status/maintenance/state"])[0] == "acknowledged")

        yp_client.update_hfsm_state(node_id, "maintenance", "Test")
        assert yp_client.get_object("node", node_id, selectors=["/status/maintenance/state"])[0] == "in_progress"

        yp_client.update_hfsm_state(node_id, "down", "Test")
        assert yp_client.get_object("node", node_id, selectors=["/status/maintenance/state"])[0] == "none"

    def test_node_segments(self, yp_env):
        yp_client = yp_env.yp_client

        node_ids = _create_nodes(yp_env, 10)
        for id in node_ids:
            yp_client.create_object("node_segment", attributes={
                "meta": {
                    "id": "segment-" + id
                },
                "spec": {
                    "node_filter": "[/labels/cool] = \"stuff\""
                }
            })

        good_node_id = node_ids[0]
        good_segment_id = "segment-" + good_node_id

        yp_client.update_object("node", good_node_id, set_updates=[{"path": "/labels/cool", "value": "stuff"}])

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": {"node_segment_id": good_segment_id}})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "enable_scheduling": True
        })

        self._wait_for_pod_assignment(yp_env)
        assert yp_client.get_object("pod", pod_id, selectors=["/status/scheduling/node_id"])[0] == good_node_id

    def test_invalid_node_filter_at_pod(self, yp_env):
        yp_client = yp_env.yp_client

        _create_nodes(yp_env, 1)
        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "enable_scheduling": True,
            "node_filter": "invalid!!!"
        })

        wait(lambda: "error" in yp_client.get_object("pod", pod_id, selectors=["/status/scheduling"])[0])

    def test_invalid_network_project_in_pod_spec(self, yp_env):
        yp_client = yp_env.yp_client

        _create_nodes(yp_env, 1)
        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, {
            "ip6_address_requests": [
                {"vlan_id": "backbone", "network_id": "nonexisting"}
            ],
            "enable_scheduling": True
        })

        wait(lambda: "error" in yp_client.get_object("pod", pod_id, selectors=["/status/scheduling"])[0])

    def _test_schedule_pod_with_exclusive_disk_usage(self, yp_env, exclusive_first):
        yp_client = yp_env.yp_client

        disk_total_capacity = 2 * (10 ** 10)
        _create_nodes(yp_env, node_count=1, disk_total_capacity=disk_total_capacity)

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})

        exclusive_pod_attributes = {
            "meta": {
                "pod_set_id": pod_set_id,
            },
            "spec": {
                "enable_scheduling": True,
                "resource_requests": ZERO_RESOURCE_REQUESTS,
                "disk_volume_requests": [
                    {
                        "id": "hdd1",
                        "storage_class": "hdd",
                        "exclusive_policy": {
                            "min_capacity": disk_total_capacity // 2,
                        },
                    },
                ],
            },
        }

        nonexclusive_pod_attributes = {
            "meta": {
                "pod_set_id": pod_set_id,
            },
            "spec": {
                "enable_scheduling": True,
                "resource_requests": ZERO_RESOURCE_REQUESTS,
                "disk_volume_requests": [
                    {
                        "id": "hdd1",
                        "storage_class": "hdd",
                        "quota_policy": {
                            "capacity": disk_total_capacity // 2,
                        },
                    },
                ],
            },
        }

        def wait_for_scheduling_status(pod_id, check_status):
            try:
                wait(lambda: check_status(_get_pod_scheduling_status(yp_client, pod_id)))
            except WaitFailed as exception:
                raise WaitFailed("Wait for pod scheduling failed: /status/scheduling = '{}')".format(
                    _get_pod_scheduling_status(yp_client, pod_id)))

        if exclusive_first:
            exclusive_pod_id = yp_client.create_object("pod", attributes=exclusive_pod_attributes)
            wait_for_scheduling_status(exclusive_pod_id, lambda status: status["state"] == "assigned" and "error" not in status)
            nonexclusive_pod_id = yp_client.create_object("pod", attributes=nonexclusive_pod_attributes)
            wait_for_scheduling_status(nonexclusive_pod_id, lambda status: status["state"] == "pending" and "error" in status)
        else:
            nonexclusive_pod_id = yp_client.create_object("pod", attributes=nonexclusive_pod_attributes)
            wait_for_scheduling_status(nonexclusive_pod_id, lambda status: status["state"] == "assigned" and "error" not in status)
            exclusive_pod_id = yp_client.create_object("pod", attributes=exclusive_pod_attributes)
            wait_for_scheduling_status(exclusive_pod_id, lambda status: status["state"] == "pending" and "error" in status)

    def test_schedule_pod_with_exclusive_disk_usage_before_nonexclusive(self, yp_env):
        self._test_schedule_pod_with_exclusive_disk_usage(yp_env, exclusive_first=True)

    def test_schedule_pod_with_exclusive_disk_usage_after_nonexclusive(self, yp_env):
        self._test_schedule_pod_with_exclusive_disk_usage(yp_env, exclusive_first=False)


@pytest.mark.usefixtures("yp_env_configurable")
class TestSchedulerEveryNodeSelectionStrategy(object):
    YP_MASTER_CONFIG = {
        "scheduler": {
            "loop_period": 100,
            "failed_allocation_backoff_time": 150,
            "global_resource_allocator": {
                "every_node_selection_strategy": {
                    "iteration_period": 5,
                    "iteration_splay": 2,
                },
            },
        },
    }

    _SCHEDULER_SAMPLE_SIZE = 10
    _NODE_COUNT = 20

    def _get_sufficient_wait_time(self):
        scheduler_config = self.YP_MASTER_CONFIG["scheduler"]
        strategy_config = scheduler_config["global_resource_allocator"]["every_node_selection_strategy"]
        max_possible_iteration_period = strategy_config["iteration_period"] + strategy_config["iteration_splay"]
        seconds_per_iteration = (scheduler_config["loop_period"] + scheduler_config["failed_allocation_backoff_time"]) / 1000.0
        return seconds_per_iteration * (max_possible_iteration_period + 5)

    def _get_default_resource_capacities(self):
        return dict(
            cpu_total_capacity=10 ** 3,
            memory_total_capacity=10 ** 10,
            disk_total_capacity=10 ** 12)

    def _test_scheduling_error(self, yp_env_configurable, pod_spec, status_check):
        yp_client = yp_env_configurable.yp_client
        _create_nodes(yp_env_configurable, node_count=self._NODE_COUNT, **self._get_default_resource_capacities())
        pod_set_id = yp_client.create_object("pod_set", attributes=dict(spec=DEFAULT_POD_SET_SPEC))
        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec)
        wait(lambda: status_check(_get_pod_scheduling_status(yp_client, pod_id)))

    def _error_status_check(self, allocation_error_string, status):
        return _is_error_pod_scheduling_status(status) and \
            "{}: {}".format(allocation_error_string, self._SCHEDULER_SAMPLE_SIZE) in str(status["error"]) and \
            "{}: {}".format(allocation_error_string, self._NODE_COUNT) in str(status["error"])

    def test_internet_address_scheduling_error(self, yp_env_configurable):
        pod_spec = dict(
            enable_scheduling=True,
            ip6_address_requests=[
                dict(
                    vlan_id="somevlan",
                    network_id="somenet",
                    enable_internet=True,
                ),
            ],
        )
        status_check = partial(self._error_status_check, "InternetAddressUnsatisfied")
        self._test_scheduling_error(yp_env_configurable, pod_spec, status_check)

    def test_antiaffinity_scheduling_error(self, yp_env_configurable):
        yp_client = yp_env_configurable.yp_client

        _create_nodes(yp_env_configurable, node_count=self._NODE_COUNT, **self._get_default_resource_capacities())

        pod_set_attributes = dict(
            spec=yt.common.update(
                DEFAULT_POD_SET_SPEC,
                dict(
                    antiaffinity_constraints=[
                        dict(
                            key="node",
                            max_pods=1,
                        ),
                    ],
                )
            )
        )
        pod_set_id = yp_client.create_object("pod_set", attributes=pod_set_attributes)

        pod_spec = dict(enable_scheduling=True)

        preallocated_pod_ids = []
        for _ in range(self._NODE_COUNT):
            preallocated_pod_ids.append(_create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec))

        def preallocated_status_check():
            statuses = map(partial(_get_pod_scheduling_status, yp_client), preallocated_pod_ids)
            return all(map(_is_assigned_pod_scheduling_status, statuses))

        wait(preallocated_status_check)

        pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec)
        wait(lambda: self._error_status_check("AntiaffinityUnsatisfied", _get_pod_scheduling_status(yp_client, pod_id)))

    def test_cpu_scheduling_error(self, yp_env_configurable):
        cpu_total_capacity = self._get_default_resource_capacities()["cpu_total_capacity"]
        pod_spec = dict(
            enable_scheduling=True,
            resource_requests=dict(vcpu_guarantee=cpu_total_capacity + 1)
        )
        status_check = partial(self._error_status_check, "CpuUnsatisfied")
        self._test_scheduling_error(yp_env_configurable, pod_spec, status_check)

    def test_memory_scheduling_error(self, yp_env_configurable):
        memory_total_capacity = self._get_default_resource_capacities()["memory_total_capacity"]
        pod_spec = dict(
            enable_scheduling=True,
            resource_requests=dict(memory_limit=memory_total_capacity + 1),
        )
        status_check = partial(self._error_status_check, "MemoryUnsatisfied")
        self._test_scheduling_error(yp_env_configurable, pod_spec, status_check)

    def test_disk_scheduling_error(self, yp_env_configurable):
        disk_total_capacity = self._get_default_resource_capacities()["disk_total_capacity"]
        pod_spec = dict(
            enable_scheduling=True,
            disk_volume_requests=[
                dict(
                    id="disk1",
                    storage_class="hdd",
                    quota_policy=dict(capacity=disk_total_capacity + 1),
                )
            ]
        )
        status_check = partial(self._error_status_check, "DiskUnsatisfied")
        self._test_scheduling_error(yp_env_configurable, pod_spec, status_check)

    def test_cpu_memory_disk_scheduling_error(self, yp_env_configurable):
        cpu_total_capacity = self._get_default_resource_capacities()["cpu_total_capacity"]
        memory_total_capacity = self._get_default_resource_capacities()["memory_total_capacity"]
        disk_total_capacity = self._get_default_resource_capacities()["disk_total_capacity"]
        pod_spec = dict(
            enable_scheduling=True,
            resource_requests=dict(
                vcpu_guarantee=cpu_total_capacity + 1,
                memory_limit=memory_total_capacity + 1,
            ),
            disk_volume_requests=[
                dict(
                    id="disk1",
                    storage_class="hdd",
                    quota_policy=dict(capacity=disk_total_capacity + 1),
                )
            ]
        )
        def status_check(status):
            for allocation_error_string in ["CpuUnsatisfied", "MemoryUnsatisfied", "DiskUnsatisfied"]:
                if not self._error_status_check(allocation_error_string, status):
                    return False
            return True
        self._test_scheduling_error(yp_env_configurable, pod_spec, status_check)

    def test_pods_collision(self, yp_env_configurable):
        yp_client = yp_env_configurable.yp_client

        _create_nodes(yp_env_configurable, node_count=self._NODE_COUNT, **self._get_default_resource_capacities())

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})

        cpu_total_capacity = self._get_default_resource_capacities()["cpu_total_capacity"]
        pod_spec = dict(
            enable_scheduling=True,
            resource_requests=dict(vcpu_guarantee=cpu_total_capacity + 1),
        )

        # Generate non-schedulable pods.
        POD_COUNT = 10
        pod_ids = []
        for i in range(POD_COUNT):
            pod_ids.append(_create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec))

        # Make sure scheduler has enough time for changing internal state.
        time.sleep(self._get_sufficient_wait_time())

        # Recreate previous pods with schedulable guarantees.
        for pod_id in pod_ids:
            yp_client.remove_object("pod", pod_id)

        pod_spec["resource_requests"]["vcpu_guarantee"] = cpu_total_capacity
        for pod_id in pod_ids:
            _create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec, pod_id=pod_id)

        # Expect scheduler to correctly identify different objects with equal ids.
        wait(lambda: all(
            _is_assigned_pod_scheduling_status(_get_pod_scheduling_status(yp_client, pod_id))
            for pod_id in pod_ids
        ))

@pytest.mark.usefixtures("yp_env_configurable")
class _TestSchedulerPodNodeScore(object):
    def test(self, yp_env_configurable):
        NODE_COUNT = 10
        POD_PER_NODE_COUNT = 10
        POD_CPU_CAPACITY = 3 * 1000
        POD_MEMORY_CAPACITY = 16 * (1024 ** 3)
        POD_DISK_CAPACITY = 100 * (1024 ** 3)

        yp_client = yp_env_configurable.yp_client

        set_account_infinite_resource_limits(yp_client, DEFAULT_ACCOUNT_ID)

        _create_nodes(
            yp_env_configurable,
            NODE_COUNT,
            rack_count=1,
            hfsm_state="up",
            cpu_total_capacity=POD_CPU_CAPACITY * POD_PER_NODE_COUNT,
            memory_total_capacity=POD_MEMORY_CAPACITY * POD_PER_NODE_COUNT,
            disk_total_capacity=POD_DISK_CAPACITY * POD_PER_NODE_COUNT,
            disk_total_volume_slots=POD_PER_NODE_COUNT,
        )

        pod_set_id = yp_client.create_object("pod_set", attributes={"spec": DEFAULT_POD_SET_SPEC})

        pod_spec = dict(
            enable_scheduling=True,
            resource_requests=dict(
                vcpu_guarantee=POD_CPU_CAPACITY,
                memory_limit=POD_MEMORY_CAPACITY,
            ),
            disk_volume_requests=[
                dict(
                    id="disk1",
                    storage_class="hdd",
                    quota_policy=dict(capacity=POD_DISK_CAPACITY),
                ),
            ],
        )

        pod_ids = []
        for pod_index in range(NODE_COUNT * POD_PER_NODE_COUNT):
            pod_ids.append(
                _create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec, pod_id=str(pod_index))
            )

        wait(lambda: all(
            _is_assigned_pod_scheduling_status(_get_pod_scheduling_status(yp_client, pod_id))
            for pod_id in pod_ids
        ))

        failed_pod_id = _create_pod_with_boilerplate(yp_client, pod_set_id, pod_spec, pod_id="failed-pod")

        wait(lambda: _is_error_pod_scheduling_status(_get_pod_scheduling_status(yp_client, failed_pod_id)))

class TestSchedulerNodeRandomHashPodNodeScore(_TestSchedulerPodNodeScore):
    YP_MASTER_CONFIG = {
        "scheduler": {
            "loop_period": 100,
            "failed_allocation_backoff_time": 150,
            "global_resource_allocator": {
                "pod_node_score": {
                    "type": "node_random_hash",
                    "parameters": {
                        "seed": 100500,
                    },
                },
            },
        },
    }

class TestSchedulerFreeCpuMemoryShareVariancePodNodeScore(_TestSchedulerPodNodeScore):
    YP_MASTER_CONFIG = {
        "scheduler": {
            "loop_period": 100,
            "failed_allocation_backoff_time": 150,
            "global_resource_allocator": {
                "pod_node_score": {
                    "type": "free_cpu_memory_share_variance",
                },
            },
        },
    }

class TestSchedulerFreeCpuMemoryShareSquaredMinDeltaPodNodeScore(_TestSchedulerPodNodeScore):
    YP_MASTER_CONFIG = {
        "scheduler": {
            "loop_period": 100,
            "failed_allocation_backoff_time": 150,
            "global_resource_allocator": {
                "pod_node_score": {
                    "type": "free_cpu_memory_share_squared_min_delta",
                },
            },
        },
    }
