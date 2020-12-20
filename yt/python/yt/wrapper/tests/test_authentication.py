from .helpers import get_tests_sandbox
from .conftest import YtTestEnvironment, authors

import yt.wrapper as yt


@authors("ignat")
def test_cypress_authentication_using_rpc_proxy():
    config = {"api_version": "v3", "backend": "rpc"}
    environment = None
    try:
        environment = YtTestEnvironment(
            get_tests_sandbox(),
            "TestAuthenticationCypress",
            config,
            env_options={"use_native_client": True},
            delta_proxy_config={"authentication": {"enable": True}})
        environment.env.create_native_client().set("//sys/tokens/abc", "pony")
        environment.env.create_native_client().create("user", attributes={"name": "pony"})
        yt.config["token"] = "abc"
        yt.config["enable_token"] = True
        assert yt.exists("/")
    finally:
        if environment is not None:
            environment.cleanup()
