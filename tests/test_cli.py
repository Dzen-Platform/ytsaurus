from .conftest import (
    Cli,
    create_nodes,
    create_pod_with_boilerplate,
    get_pod_scheduling_status,
    is_assigned_pod_scheduling_status,
)

from yp.common import wait

import yt.yson as yson

from yt.packages.six.moves import xrange

try:
    import yt.json_wrapper as json
except ImportError:
    import yt.json as json

import pytest


class YpCli(Cli):
    def __init__(self, grpc_address):
        super(YpCli, self).__init__("python/yp/bin", "yp_make", "yp")
        self.set_env_patch(dict(YP_ADDRESS=grpc_address, YP_PROTOCOL="grpc"))
        self.set_config(dict(enable_ssl=False))

    def set_config(self, config):
        self._config = yson.dumps(config, yson_format="text")

    def get_args(self, args):
        return super(YpCli, self).get_args(args) + ["--config", self._config]

    def check_yson_output(self, *args, **kwargs):
        result = self.check_output(*args, **kwargs)
        return yson._loads_from_native_str(result)

def create_cli(yp_env):
    return YpCli(yp_env.yp_instance.yp_client_grpc_address)


def create_pod(cli):
    pod_set_id = cli.check_output(["create", "pod_set"])
    attributes = {"meta": {"pod_set_id": pod_set_id}}
    return cli.check_output([
        "create",
        "pod",
        "--attributes", yson.dumps(attributes)
    ])

def create_user(cli):
    return cli.check_output(["create", "user"])


@pytest.mark.usefixtures("yp_env")
class TestCli(object):
    def test_common(self, yp_env):
        cli = create_cli(yp_env)

        pod_id = create_pod(cli)

        result = cli.check_yson_output([
            "get",
            "pod", pod_id,
            "--selector", "/status/agent/state",
            "--selector", "/meta/id"
        ])
        assert result == ["unknown", pod_id]

        result = cli.check_yson_output([
            "select",
            "pod",
            "--filter", '[/meta/id] = "{}"'.format(pod_id),
            "--no-tabular"
        ])
        assert result == [[]]

    def test_check_object_permission(self, yp_env):
        cli = create_cli(yp_env)

        pod_id = create_pod(cli)

        result = cli.check_yson_output([
            "check-object-permission",
            "pod", pod_id,
            "everyone",
            "read"
        ])
        assert result == dict(action="deny")

        result = cli.check_yson_output([
            "check-permission",
            "pod", pod_id,
            "root",
            "write"
        ])
        assert result == dict(action="allow")

        user_id = create_user(cli)
        yp_env.sync_access_control()

        result = cli.check_yson_output([
            "check-permission",
            "pod", pod_id,
            user_id,
            "read"
        ])
        assert result["action"] == "allow"

    def test_get_object_access_allowed_for(self, yp_env):
        cli = create_cli(yp_env)

        pod_id = create_pod(cli)

        all_user_ids = ["root"]
        for _ in xrange(10):
            all_user_ids.append(create_user(cli))

        result = cli.check_yson_output([
            "get-object-access-allowed-for",
            "pod", pod_id,
            "read"
        ])

        assert "user_ids" in result
        result["user_ids"].sort()

        assert result == dict(user_ids=sorted(all_user_ids))

    def test_get_user_access_allowed_to(self, yp_env):
        cli = create_cli(yp_env)

        yp_env.sync_access_control()

        result = cli.check_yson_output([
            "get-user-access-allowed-to",
            "root",
            "account",
            "read"
        ])

        assert result == dict(object_ids=["tmp"])

    def test_binary_data(self, yp_env):
        cli = create_cli(yp_env)

        pod_set_id = cli.check_output([
            "create", "pod_set",
            "--attributes", yson.dumps({"annotations": {"hello": "\x01\x02"}})
        ])

        result = cli.check_yson_output([
            "get",
            "pod_set", pod_set_id,
            "--selector", "/annotations",
        ])
        assert result == [{"hello": "\x01\x02"}]

        result = cli.check_output([
            "get",
            "pod_set", pod_set_id,
            "--selector", "/annotations",
            "--format", "json",
        ])
        assert json.loads(result) == [{"hello": "\x01\x02"}]

    def test_update_hfsm(self, yp_env):
        cli = create_cli(yp_env)

        node_id = yp_env.yp_client.create_object("node")
        cli.check_output(["update-hfsm-state", node_id, "up", "test"])

        result = cli.check_yson_output([
            "get",
            "node", node_id,
            "--selector", "/status/hfsm/state"
        ])

        assert result[0] == "up"

    def test_pod_eviction(self, yp_env):
        cli = create_cli(yp_env)
        yp_client = yp_env.yp_client

        create_nodes(yp_client, 1)[0]
        pod_set_id = yp_client.create_object("pod_set")
        pod_id = create_pod_with_boilerplate(yp_client, pod_set_id, dict(enable_scheduling=True))
        wait(lambda: is_assigned_pod_scheduling_status(get_pod_scheduling_status(yp_client, pod_id)))

        get_eviction_state = lambda: cli.check_yson_output([
            "get",
            "pod", pod_id,
            "--selector", "/status/eviction/state"
        ])[0]
        get_eviction_message = lambda: cli.check_yson_output([
            "get",
            "pod", pod_id,
            "--selector", "/status/eviction/message"
        ])[0]

        assert get_eviction_state() == "none"

        message = "hello, eviction!"
        cli.check_output(["request-eviction", pod_id, message])
        assert get_eviction_state() == "requested"
        assert get_eviction_message() == message

        cli.check_output(["abort-eviction", pod_id, "test"])
        assert get_eviction_state() == "none"

        cli.check_output(["request-eviction", pod_id, "test"])
        assert get_eviction_state() == "requested"

        tx_id = yp_client.start_transaction()
        cli.check_output([
            "acknowledge-eviction",
            pod_id, "test",
            "--transaction_id", tx_id
        ])
        assert get_eviction_state() == "requested"
        yp_client.commit_transaction(tx_id)
        assert get_eviction_state() in ("acknowledged", "none")
