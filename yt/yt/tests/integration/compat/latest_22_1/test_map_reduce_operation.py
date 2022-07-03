from original_tests.yt.yt.tests.integration.controller.test_map_reduce_operation \
    import TestSchedulerMapReduceCommands as BaseTestMapReduceCommands
from yt.common import update


class TestMapReduceCommandsCompatNewCA(BaseTestMapReduceCommands):
    ARTIFACT_COMPONENTS = {
        "22_1": ["master", "node", "job-proxy", "exec", "tools"],
        "trunk": ["scheduler", "controller-agent", "proxy", "http-proxy"],
    }

    DELTA_CONTROLLER_AGENT_CONFIG = update(BaseTestMapReduceCommands.DELTA_CONTROLLER_AGENT_CONFIG, {
        "controller_agent": {
            "enable_table_column_renaming": False,
            "operation_options": {
                "spec_template": {
                    "enable_table_index_if_has_trivial_mapper": True,
                },
            },
        },
    })


class TestMapReduceCommandsCompatNewNodes(BaseTestMapReduceCommands):
    ARTIFACT_COMPONENTS = {
        "22_1": ["master", "scheduler", "controller-agent"],
        "trunk": ["node", "job-proxy", "exec", "tools", "proxy", "http-proxy"],
    }

    DELTA_CONTROLLER_AGENT_CONFIG = update(BaseTestMapReduceCommands.DELTA_CONTROLLER_AGENT_CONFIG, {
        "controller_agent": {
            "enable_table_column_renaming": False,
            "operation_options": {
                "spec_template": {
                    "enable_table_index_if_has_trivial_mapper": True,
                },
            },
        },
    })
