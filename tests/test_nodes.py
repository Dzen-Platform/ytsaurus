from .conftest import ZERO_RESOURCE_REQUESTS

from yp.common import YtResponseError

from yt.yson import YsonEntity

import pytest


@pytest.mark.usefixtures("yp_env")
class TestNodes(object):
    def test_create_get(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = "node.yandex.ru"
        assert (
            yp_client.create_object(
                object_type="node", attributes={"meta": {"id": "node.yandex.ru"}}
            )
            == node_id
        )
        result = yp_client.get_object("node", node_id, selectors=["/meta/id", "/spec/short_name"])
        assert result == [node_id, "node"]

    def test_invalid_ip6_subnet(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YtResponseError):
            yp_client.create_object(
                object_type="node",
                attributes={
                    "spec": {"ip6_subnets": [{"subnet": "blablabla", "vlan_id": "somevlan"}]}
                },
            )

    def test_invalid_ip6_address(self, yp_env):
        yp_client = yp_env.yp_client

        with pytest.raises(YtResponseError):
            yp_client.create_object(
                object_type="node",
                attributes={
                    "spec": {"ip6_addresses": [{"address": "blablabla", "vlan_id": "somevlan"}]}
                },
            )

    def test_cannot_remove_node_with_pods(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = yp_client.create_object(object_type="node")
        pod_set_id = yp_client.create_object(object_type="pod_set")
        pod_id = yp_client.create_object(
            object_type="pod",
            attributes={
                "meta": {"pod_set_id": pod_set_id},
                "spec": {"resource_requests": ZERO_RESOURCE_REQUESTS, "node_id": node_id},
            },
        )

        with pytest.raises(YtResponseError):
            yp_client.remove_object("node", node_id)
        yp_client.remove_object("pod", pod_id)
        yp_client.remove_object("node", node_id)

    def test_update_hfsm_state(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = yp_client.create_object(object_type="node")
        assert (
            yp_client.get_object("node", node_id, selectors=["/status/hfsm/state"])[0] == "initial"
        )

        yp_client.update_object(
            "node",
            node_id,
            set_updates=[
                {"path": "/control/update_hfsm_state", "value": {"state": "up", "message": "test"}}
            ],
        )
        assert yp_client.get_object("node", node_id, selectors=["/status/hfsm/state"])[0] == "up"

    def test_host_spec(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = yp_client.create_object(object_type="node")
        yp_client.get_object("node", node_id, selectors=["/spec/host_manager"])[0]

        PAYLOAD = {"type_url": "yandex.ru/someurl", "value": "somevalue"}
        yp_client.update_object(
            "node", node_id, set_updates=[{"path": "/spec/host_manager", "value": PAYLOAD}]
        )
        assert yp_client.get_object("node", node_id, selectors=["/spec/host_manager"])[0] == PAYLOAD

    def test_host_status(self, yp_env):
        yp_client = yp_env.yp_client

        node_id = yp_client.create_object(object_type="node")
        yp_client.get_object("node", node_id, selectors=["/status/host_manager"])[0]
