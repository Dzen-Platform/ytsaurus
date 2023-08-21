from base import ClickHouseTestBase, Clique

from yt_commands import (authors, print_debug)

from yt.common import wait

import time


class TestQueryLog(ClickHouseTestBase):
    @staticmethod
    def _get_query_log_patch(query_log_period, query_log_older_period):
        return {
            "clickhouse": {
                "query_log": {
                    "engine": f"ENGINE = Buffer('system', 'query_log_older', 1, 1, {query_log_period}, "
                              "1000000000000, 1000000000000, 1000000000000, 1000000000000)",
                },
            },
            "yt": {
                "query_log": {
                    "additional_tables": [
                        {
                            "database": "system",
                            "name": "query_log_older",
                            "engine": f"ENGINE = Buffer('', '', 1, 1, {query_log_older_period}, "
                                      "1000000000000, 1000000000000, 1000000000000, 1000000000000)",
                        },
                    ],
                },
            },
        }

    @authors("max42")
    def test_query_log_simple(self):
        with Clique(1, config_patch=self._get_query_log_patch(1.5, 1)) as clique:
            clique.make_query("select 1")
            wait(lambda: len(clique.make_query("select * from system.query_log")) >= 1)
            time.sleep(6)
            assert len(clique.make_query("select * from system.query_log")) == 0

    @authors("max42", "dakovalkov")
    def test_query_log_eviction(self):
        query_log_flush_period = 3
        query_log_older_flush_period = 2

        with Clique(1, config_patch=self._get_query_log_patch(query_log_flush_period, query_log_older_flush_period)) as clique:
            timespan = 15
            counter = 0
            start = time.time()

            identifier_to_state = dict()

            while True:
                now = time.time() - start
                if now > timespan:
                    break
                identifier = "foo{:03}".format(counter)
                assert clique.make_query("select 1 as {}".format(identifier), verbose=False)[0] == {identifier: 1}
                counter += 1
                identifier_to_state[identifier] = {"state": "unseen", "started_at": now}

                all_entries = clique.make_query("select * from system.query_log", verbose=False)
                # NB: Flushing buffer table is not atomic. Query can be temporary missing from the log
                # if it's being transferred from query_log to query_log_older during "select * from system.query_log" query.
                # To guarantee that all queries are present in the result, we run the query two times.
                all_entries.extend(clique.make_query("select * from system.query_log", verbose=False))

                seen_now = {None}
                for entry in all_entries:
                    if not entry["query"]:
                        continue
                    foo_index = entry["query"].find("foo")
                    if foo_index != -1:
                        found_identifier = entry["query"][foo_index:foo_index + 6]
                        seen_now.add(found_identifier)
                        state = identifier_to_state[found_identifier]
                        assert state["state"] in ("unseen", "seen")
                        if state["state"] == "unseen":
                            state["state"] = "seen"
                            state["seen_at"] = now
                for identifier, state in identifier_to_state.items():
                    if identifier in seen_now:
                        continue
                    if state["state"] == "seen":
                        state["state"] = "evicted"
                        state["evicted_at"] = now

        for identifier, state in sorted(identifier_to_state.items(), key=lambda pair: pair[0]):
            print_debug(identifier, state)
            if state["state"] in ("seen", "evicted"):
                assert state["seen_at"] - state["started_at"] <= 0.5
            if state["state"] == "evicted":
                assert state["evicted_at"] - state["started_at"] >= query_log_older_flush_period - 1
                assert state["evicted_at"] - state["started_at"] <= (query_log_flush_period + query_log_older_flush_period) + 3

            if state["started_at"] <= timespan - (query_log_flush_period + query_log_older_flush_period) - 3:
                assert state["state"] == "evicted"
            if state["started_at"] <= timespan - 2:
                assert state["state"] in ("seen", "evicted")
