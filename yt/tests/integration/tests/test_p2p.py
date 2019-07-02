from yt_env_setup import YTEnvSetup, Restarter, NODES_SERVICE
from yt_commands import *
from yt_helpers import ProfileMetric

from flaky import flaky

def clear_everything_after_test(func):
    def wrapped(*args, **kwargs):
        func(*args, **kwargs)
        self = args[0]
        with Restarter(self.Env, NODES_SERVICE):
            pass
        nodes = list(get("//sys/cluster_nodes"))
        for node in nodes:
            set("//sys/cluster_nodes/{0}/@user_tags".format(node), [])
    return wrapped

class TestBlockPeerDistributorSynthetic(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 4
    NUM_SCHEDULERS = 0

    DELTA_NODE_CONFIG = {
        "data_node": {
            "peer_block_distributor": {
                "iteration_period": 100, # 0.1 sec
                "window_length": 1000, # 1 sec,
                # In tests we are always trying to distribute something.
                "out_traffic_activation_threshold": 0,
                "node_tag_filter": "!tag42",
                "min_request_count": 3,
                "max_distribution_count": 1,
                "destination_node_count": 2,
            },
            "block_cache": {
                "compressed_data": {
                    "capacity": 256 * 1024 * 1024,
                }
            }
        }
    }

    DELTA_DRIVER_CONFIG = {
        "node_directory_synchronizer": {
            "sync_period": 50 # To force NodeDirectorySynchronizer in tests
        }
    }

    def setup(self):
        create("table", "//tmp/t")
        set("//tmp/t/@replication_factor", 1)
        write_table("//tmp/t", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t")
        # Node that is the seed for the only existing chunk.
        self.seed = str(get("#{0}/@stored_replicas/0".format(chunk_id)))
        self.nodes = ls("//sys/cluster_nodes")
        self.non_seeds = ls("//sys/cluster_nodes")
        self.non_seeds.remove(self.seed)
        assert len(self.non_seeds) == 3
        print >>sys.stderr, "Seed: ", self.seed
        print >>sys.stderr, "Non-seeds: ", self.non_seeds

    @classmethod
    def _access(cls):
        read_table("//tmp/t")

    @clear_everything_after_test
    def test_no_distribution(self):
        with ProfileMetric.at_node(self.seed, "data_node/p2p/distributed_block_size") as p:
            # Keep number of tries in sync with min_request_count.
            self._access()
            self._access()
            time.sleep(2)
        assert p.differentiate() == 0

    @flaky(max_runs=5)
    @clear_everything_after_test
    def test_simple_distribution(self):
        with ProfileMetric.at_node(self.seed, "data_node/p2p/distributed_block_size") as p:
            # Must be greater than min_request_count in config.
            self._access()
            self._access()
            self._access()
            self._access()
            self._access()
            time.sleep(2)
        assert p.differentiate() > 0

    @clear_everything_after_test
    def test_node_filter_tags(self):
        for non_seed in self.non_seeds:
            set("//sys/cluster_nodes/{0}/@user_tags".format(non_seed), ["tag42"])
        # Wait for node directory to become updated.
        time.sleep(2)
        with ProfileMetric.at_node(self.seed, "data_node/p2p/distributed_block_size") as p:
            self._access()
            self._access()
            self._access()
            time.sleep(2)
        assert p.differentiate() == 0

class TestBlockPeerDistributorManyRequestsProduction(TestBlockPeerDistributorSynthetic):
    DELTA_NODE_CONFIG = {
        "data_node": {
            "peer_block_distributor": {
                "iteration_period": 100, # 0.1 sec
                "window_length": 1000, # 1 sec,
                # In tests we are always trying to distribute something.
                "out_traffic_activation_threshold": 0,
                "node_tag_filter": "!tag42",
                "min_request_count": 3,
                "max_distribution_count": 12, # As in production
                "destination_node_count": 2,
                "consecutive_distribution_delay": 200,
            },
            "block_cache": {
                "compressed_data": {
                    "capacity": 256 * 1024 * 1024,
                }
            }
        }
    }
    DELTA_DRIVER_CONFIG = {
        "node_directory_synchronizer": {
            "sync_period": 50 # To force NodeDirectorySynchronizer in tests
        }
    }

    # Test relies on timing of rpc calls and periods of node directory synchronizer and distribution iteration.
    @flaky(max_runs=5)
    @clear_everything_after_test
    def test_wow_block_so_hot_such_many_requests(self):
        with ProfileMetric.at_node(self.seed, "data_node/block_cache/compressed_data/hit") as ps, \
            ProfileMetric.at_node(self.non_seeds[0], "data_node/block_cache/compressed_data/hit") as pns0, \
            ProfileMetric.at_node(self.non_seeds[1], "data_node/block_cache/compressed_data/hit") as pns1, \
            ProfileMetric.at_node(self.non_seeds[2], "data_node/block_cache/compressed_data/hit") as pns2:

            for i in range(300):
                self._access()
        assert ps.differentiate() > 0
        assert pns0.differentiate() > 0
        assert pns1.differentiate() > 0
        assert pns2.differentiate() > 0
