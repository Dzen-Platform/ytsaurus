from yt_env_setup import YTEnvSetup, Restarter, NODES_SERVICE

from yt_commands import (
    authors, wait, create, ls, get, set, remove, link, exists,
    write_file, write_table, get_job,
    map, wait_for_nodes)

from yt.common import YtError
import yt.yson as yson

from yt_helpers import profiler_factory

import pytest

import os
import time


class TestLayers(YTEnvSetup):
    NUM_SCHEDULERS = 1
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        }
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1")
        file_name = "layers/static-bin.tar.gz"
        write_file("//tmp/layer1", open(file_name, "rb").read())

        create("file", "//tmp/layer2")
        file_name = "layers/test.tar.gz"
        write_file("//tmp/layer2", open(file_name, "rb").read())

        create("file", "//tmp/corrupted_layer")
        file_name = "layers/corrupted.tar.gz"
        write_file("//tmp/corrupted_layer", open(file_name, "rb").read())

        create("file", "//tmp/static_cat")
        file_name = "layers/static_cat"
        write_file("//tmp/static_cat", open(file_name, "rb").read())

        set("//tmp/static_cat/@executable", True)

    @authors("ilpauzner")
    def test_disabled_layer_locations(self):
        with Restarter(self.Env, NODES_SERVICE):
            disabled_path = None
            for node in self.Env.configs["node"][:1]:
                for layer_location in node["data_node"]["volume_manager"]["layer_locations"]:
                    try:
                        disabled_path = layer_location["path"]
                        os.mkdir(layer_location["path"])
                    except OSError:
                        pass
                    with open(layer_location["path"] + "/disabled", "w"):
                        pass

        wait_for_nodes()

        with Restarter(self.Env, NODES_SERVICE):
            os.unlink(disabled_path + "/disabled")
        wait_for_nodes()

        time.sleep(5)

    @authors("prime")
    def test_corrupted_layer(self):
        self.setup_files()
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        with pytest.raises(YtError):
            map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="./static_cat; ls $YT_ROOT_FS 1>&2",
                file="//tmp/static_cat",
                spec={
                    "max_failed_job_count": 1,
                    "mapper": {
                        "layer_paths": ["//tmp/layer1", "//tmp/corrupted_layer"],
                    },
                },
            )

        # YT-14186: Corrupted user layer should not disable jobs on node.
        for node in ls("//sys/cluster_nodes"):
            assert len(get("//sys/cluster_nodes/{}/@alerts".format(node))) == 0

    @authors("psushin")
    def test_one_layer(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                },
            },
        )

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        for job_id in job_ids:
            assert b"static-bin" in op.read_stderr(job_id)

    @authors("psushin")
    def test_two_layers(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1", "//tmp/layer2"],
                },
            },
        )

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        for job_id in job_ids:
            stderr = op.read_stderr(job_id)
            assert b"static-bin" in stderr
            assert b"test" in stderr

    @authors("psushin")
    def test_bad_layer(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        with pytest.raises(YtError):
            map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="./static_cat; ls $YT_ROOT_FS 1>&2",
                file="//tmp/static_cat",
                spec={
                    "max_failed_job_count": 1,
                    "mapper": {
                        "layer_paths": ["//tmp/layer1", "//tmp/bad_layer"],
                    },
                },
            )

    @authors("prime")
    def test_squashfs_spec_options(self):
        self.setup_files()

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                },
                "enable_squashfs": True,
            },
        )

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        for job_id in job_ids:
            assert b"static-bin" in op.read_stderr(job_id)


@authors("psushin")
class TestTmpfsLayerCache(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
        "data_node": {
            "volume_manager": {
                "regular_tmpfs_layer_cache": {
                    "capacity": 10 * 1024 * 1024,
                    "layers_update_period": 100,
                },
                "nirvana_tmpfs_layer_cache": {
                    "capacity": 10 * 1024 * 1024,
                    "layers_update_period": 100,
                }
            }
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file("//tmp/layer1", open(file_name, "rb").read())

        create("file", "//tmp/static_cat", attributes={"replication_factor": 1})
        file_name = "layers/static_cat"
        write_file("//tmp/static_cat", open(file_name, "rb").read())

        set("//tmp/static_cat/@executable", True)

    def test_tmpfs_layer_cache(self):
        self.setup_files()

        orchid_path = "orchid/job_controller/slot_manager/root_volume_manager"

        for node in ls("//sys/cluster_nodes"):
            assert get("//sys/cluster_nodes/{0}/{1}/regular_tmpfs_cache/layer_count".format(node, orchid_path)) == 0
            assert get("//sys/cluster_nodes/{0}/{1}/nirvana_tmpfs_cache/layer_count".format(node, orchid_path)) == 0

        create("map_node", "//tmp/cached_layers")
        link("//tmp/layer1", "//tmp/cached_layers/layer1")

        with Restarter(self.Env, NODES_SERVICE):
            # First we create cypress map node for cached layers,
            # and then add it to node config with node restart.
            # Otherwise environment starter will consider node as dead, since
            # it will not be able to initialize tmpfs layer cache and will
            # report zero user job slots.
            for i, config in enumerate(self.Env.configs["node"]):
                config["data_node"]["volume_manager"]["regular_tmpfs_layer_cache"]["layers_directory_path"] = "//tmp/cached_layers"
                config["data_node"]["volume_manager"]["nirvana_tmpfs_layer_cache"]["layers_directory_path"] = "//tmp/cached_layers"
                config_path = self.Env.config_paths["node"][i]
                with open(config_path, "wb") as fout:
                    yson.dump(config, fout)

        wait_for_nodes()
        for node in ls("//sys/cluster_nodes"):
            # After node restart we must wait for async root volume manager initialization.
            wait(lambda: exists("//sys/cluster_nodes/{0}/{1}".format(node, orchid_path)))
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/regular_tmpfs_cache/layer_count".format(node, orchid_path)) == 1)
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/nirvana_tmpfs_cache/layer_count".format(node, orchid_path)) == 1)

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0, "u": 1, "v": 2}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./static_cat; ls $YT_ROOT_FS 1>&2",
            file="//tmp/static_cat",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                },
            },
        )

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        job_id = job_ids[0]
        assert b"static-bin" in op.read_stderr(job_id)

        job = get_job(op.id, job_id)
        regular_cache_hits = profiler_factory().at_node(job["address"]).get("exec_node/layer_cache/tmpfs_cache_hits", {"cache_name": "regular"})
        nirvana_cache_hits = profiler_factory().at_node(job["address"]).get("exec_node/layer_cache/tmpfs_cache_hits", {"cache_name": "nirvana"})

        assert regular_cache_hits > 0 or nirvana_cache_hits > 0

        remove("//tmp/cached_layers/layer1")
        for node in ls("//sys/cluster_nodes"):
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/regular_tmpfs_cache/layer_count".format(node, orchid_path)) == 0)
            wait(lambda: get("//sys/cluster_nodes/{0}/{1}/nirvana_tmpfs_cache/layer_count".format(node, orchid_path)) == 0)


@authors("mrkastep")
class TestJobSetup(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "job_controller": {
                "job_setup_command": {
                    "path": "/static-bin/static-bash",
                    "args": ["-c", "echo SETUP-OUTPUT > /setup_output_file"],
                }
            },
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },
    }

    USE_PORTO = True

    def setup_files(self):
        create("file", "//tmp/layer1", attributes={"replication_factor": 1})
        file_name = "layers/static-bin.tar.gz"
        write_file(
            "//tmp/layer1",
            open(file_name, "rb").read(),
            file_writer={"upload_replication_factor": 1},
        )

    def test_setup_cat(self):
        self.setup_files()

        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        write_table("//tmp/t_in", [{"k": 0}])
        op = map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="$YT_ROOT_FS/static-bin/static-cat $YT_ROOT_FS/setup_output_file >&2",
            spec={
                "max_failed_job_count": 1,
                "mapper": {
                    "layer_paths": ["//tmp/layer1"],
                    "job_count": 1,
                },
            },
        )

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        job_id = job_ids[0]

        res = op.read_stderr(job_id)
        assert res == b"SETUP-OUTPUT\n"


@authors("prime")
class TestSquashfsLayers(TestLayers):
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },

        "data_node": {
            "volume_manager": {
                "convert_layers_to_squashfs": True,
                "use_bundled_tar2squash": True,
            }
        },
    }


@authors("prime")
class TestSquashfsTmpfsLayerCache(TestTmpfsLayerCache):
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "test_root_fs": True,
            "slot_manager": {
                "job_environment": {
                    "type": "porto",
                },
            },
        },

        "data_node": {
            "volume_manager": {
                "convert_layers_to_squashfs": True,
                "use_bundled_tar2squash": True,

                "regular_tmpfs_layer_cache": {
                    "capacity": 10 * 1024 * 1024,
                    "layers_update_period": 100,
                },
                "nirvana_tmpfs_layer_cache": {
                    "capacity": 10 * 1024 * 1024,
                    "layers_update_period": 100,
                }
            }
        },
    }
