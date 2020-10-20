import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *

from flaky import flaky

##################################################################


class TestMasterCellsSync(YTEnvSetup):
    START_SECONDARY_MASTER_CELLS = True
    ENABLE_SECONDARY_CELLS_CLEANUP = False
    NUM_SECONDARY_MASTER_CELLS = 2
    NUM_NODES = 3

    DELTA_MASTER_CONFIG = {
        "tablet_manager": {
            "leader_reassignment_timeout": 2000,
            "peer_revocation_timeout": 3000,
        },
    }

    @classmethod
    def setup_class(cls, delayed_secondary_cells_start=False):
        super(TestMasterCellsSync, cls).setup_class()
        cls.delayed_secondary_cells_start = delayed_secondary_cells_start

    def _check_true_for_secondary(self, check):
        if self.delayed_secondary_cells_start:
            self.Env.start_secondary_master_cells()
        try:

            def _check():
                for i in xrange(self.Env.secondary_master_cell_count):
                    if not check(get_driver(i + 1)):
                        return False
                return true
                wait(_check)

        finally:
            if self.delayed_secondary_cells_start:
                for cell_index in xrange(self.Env.secondary_master_cell_count):
                    self.Env.kill_master_cell(cell_index + 1)

    def teardown(self):
        if self.delayed_secondary_cells_start:
            for cell_index in xrange(self.Env.secondary_master_cell_count):
                self.Env.start_master_cell(cell_index + 1)

    @authors("asaitgalin")
    def test_users_sync(self):
        create_user("tester")

        for i in xrange(10):
            set("//sys/users/tester/@custom{0}".format(i), "value")
        self._check_true_for_secondary(
            lambda driver: all(
                [get("//sys/users/tester/@custom{0}".format(i), driver=driver) == "value" for i in xrange(10)]
            )
        )
        self._check_true_for_secondary(lambda driver: "tester" in ls("//sys/users", driver=driver))

        remove_user("tester")
        self._check_true_for_secondary(lambda driver: "tester" not in ls("//sys/users", driver=driver))

    @authors("asaitgalin")
    def test_groups_sync(self):
        create_user("tester")
        create_group("sudoers")
        add_member("tester", "sudoers")

        self._check_true_for_secondary(lambda driver: "sudoers" in ls("//sys/groups", driver=driver))
        self._check_true_for_secondary(lambda driver: "tester" in get("//sys/groups/sudoers/@members", driver=driver))
        self._check_true_for_secondary(lambda driver: "sudoers" in get("//sys/users/tester/@member_of", driver=driver))

        for i in xrange(10):
            set("//sys/groups/sudoers/@attr{0}".format(i), "value")
        remove_member("tester", "sudoers")

        check_attributes = lambda driver: all(
            [get("//sys/groups/sudoers/@attr{0}".format(i), driver=driver) == "value" for i in xrange(10)]
        )
        check_membership = lambda driver: "tester" not in get("//sys/groups/sudoers/@members", driver=driver)

        self._check_true_for_secondary(lambda driver: check_attributes(driver) and check_membership(driver))
        remove_group("sudoers")
        self._check_true_for_secondary(lambda driver: "sudoers" not in ls("//sys/groups", driver=driver))

    @authors("asaitgalin")
    def test_accounts_sync(self):
        create_account("tst", sync_creation=False)

        for i in xrange(10):
            set("//sys/accounts/tst/@attr{0}".format(i), "value")
        self._check_true_for_secondary(
            lambda driver: all(
                [get("//sys/accounts/tst/@attr{0}".format(i), driver=driver) == "value" for i in xrange(10)]
            )
        )

        remove_account("tst")
        self._check_true_for_secondary(lambda driver: "tst" not in ls("//sys/accounts", driver=driver))

    @authors("asaitgalin")
    def test_schemas_sync(self):
        create_group("testers")

        for subj in ["user", "account", "table"]:
            set(
                "//sys/schemas/{0}/@acl/end".format(subj),
                make_ace("allow", "testers", "create"),
            )

        def check(driver):
            ok = True
            for subj in ["user", "account"]:
                found = False
                for acl in get("//sys/schemas/{0}/@acl".format(subj), driver=driver):
                    if "testers" in acl["subjects"]:
                        found = True
                ok = ok and found
            return ok

        self._check_true_for_secondary(lambda driver: check(driver))

    @authors("babenko")
    def test_acl_sync(self):
        create_group("jupiter")
        create_account("jupiter", sync_creation=False)
        set("//sys/accounts/jupiter/@acl", [make_ace("allow", "jupiter", "use")])

        def check(driver):
            return len(get("//sys/accounts/jupiter/@acl", driver=driver)) == 1

        self._check_true_for_secondary(lambda driver: check(driver))

    @authors("babenko")
    def test_rack_sync(self):
        create_rack("r")

        def check(driver):
            return exists("//sys/racks/r")

        self._check_true_for_secondary(lambda driver: check(driver))

    @authors("savrus")
    def test_tablet_cell_bundle_sync(self):
        create_tablet_cell_bundle("b")

        for i in xrange(10):
            set("//sys/tablet_cell_bundles/b/@custom{0}".format(i), "value")
        self._check_true_for_secondary(
            lambda driver: all(
                [
                    get(
                        "//sys/tablet_cell_bundles/b/@custom{0}".format(i),
                        driver=driver,
                    )
                    == "value"
                    for i in xrange(10)
                ]
            )
        )

        self._check_true_for_secondary(lambda driver: "b" in ls("//sys/tablet_cell_bundles", driver=driver))

        remove_tablet_cell_bundle("b")
        self._check_true_for_secondary(lambda driver: "b" not in ls("//sys/tablet_cell_bundles", driver=driver))

    @authors("savrus")
    @flaky(max_runs=5)
    def test_tablet_cell_sync(self):
        create_tablet_cell_bundle("b")
        set(
            "//sys/tablet_cell_bundles/b/@dynamic_options/suppress_tablet_cell_decommission",
            True,
        )
        cell_id = create_tablet_cell(attributes={"tablet_cell_bundle": "b"})
        wait_for_cells()

        def _get_peer_address(cell_id):
            return get("#{0}/@peers/0/address".format(cell_id), default=None)

        peer = _get_peer_address(cell_id)
        set("//sys/cluster_nodes/{0}/@disable_tablet_cells".format(peer), True)
        wait(lambda: _get_peer_address(cell_id) != peer)

        remove("#{0}".format(cell_id))

        config_version = get("#{0}/@config_version".format(cell_id), read_from="leader")
        wait_for_cells()
        assert config_version > 2

        def check(driver):
            return (
                get(
                    "//sys/tablet_cells/{0}/@tablet_cell_bundle".format(cell_id),
                    driver=driver,
                )
                == "b"
                and get(
                    "#{0}/@config_version".format(cell_id),
                    driver=driver,
                    read_from="leader",
                )
                == config_version
                and get(
                    "#{0}/@tablet_cell_life_stage".format(cell_id),
                    driver=driver,
                    read_from="leader",
                )
                == "decommissioned"
            )

        self._check_true_for_secondary(lambda driver: check(driver))

    @authors("asaitgalin", "savrus")
    def test_safe_mode_sync(self):
        set("//sys/@config/enable_safe_mode", True)

        def check(driver, value):
            return get("//sys/@config/enable_safe_mode", driver=driver) == value

        self._check_true_for_secondary(lambda driver: check(driver, True))
        set("//sys/@config", {})
        self._check_true_for_secondary(lambda driver: check(driver, False))


##################################################################


@pytest.mark.skipif("True", reason="Currently broken")
class TestMasterCellsSyncDelayed(TestMasterCellsSync):
    START_SECONDARY_MASTER_CELLS = False

    @classmethod
    def setup_class(cls):
        super(TestMasterCellsSyncDelayed, cls).setup_class(delayed_secondary_cells_start=True)
