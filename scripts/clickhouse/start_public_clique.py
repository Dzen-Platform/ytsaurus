#!/usr/bin/python2.7

import argparse
import sys
import logging
import yt.wrapper as yt
import yt.yson as yson

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)
handler = logging.StreamHandler()
handler.setFormatter(logging.Formatter("%(asctime)s  %(levelname).1s  %(module)s:%(lineno)d  %(message)s"))
logger.addHandler(handler)

def main():
    parser = argparse.ArgumentParser(description="Restart public (or its equivalent, for example, prestable) clique")
    parser.add_argument("-b", "--bin", default="//sys/clickhouse/bin/ytserver-clickhouse", help="ytserver-clickhouse binary to use")
    parser.add_argument("-t", "--type", default="public", help="Clique type; for example 'public' or 'prestable'")
    parser.add_argument("-g", "--graceful-preemption", action="store_true", help="Enable graceful preemption")
    parser.add_argument("--skip-wrapper-version-check", action="store_true", help="NOT RECOMMENDED: try to run script with yt wrapper from package rather than from working copy")
    args = parser.parse_args()

    if args.skip_wrapper_version_check:
        logger.warning("Running with wrapper version %s, it may miss some important clickhouse functional to run public cliques", yt.VERSION)
    elif yt.VERSION != "unknown":
        logger.error("Running with wrapper not from working copy (wrapper version %s) is not recommended; if you still want to proceed, use --skip-wrapper-version-check flag", yt.VERSION)
        sys.exit(1)

    assert args.type in ("prestable", "datalens", "public")

    alias = "*ch_" + args.type

    yt.start_clickhouse_clique(
        16 if args.type != "prestable" else 4,
        alias,
        cpu_limit=8,
        enable_monitoring=True,
        enable_job_tables=True,
        enable_log_tailer=True,
        cypress_geodata_path="//sys/clickhouse/geodata/geodata.tgz",
        cypress_ytserver_clickhouse_path=args.bin,
        spec={
            "acl": [{
                "subjects": ["yandex"],
                "permissions": ["read"],
                "action": "allow"
            }],
            "title": args.type.capitalize() + " clique",
            "pool": "chyt",
            "preemption_mode": "graceful" if args.graceful_preemption else "normal",
        },
        clickhouse_config={
            "validate_operation_access": False,
            "engine": {
                "settings": {
                    "max_execution_time": 600 if args.type == "datalens" else 1800,
                },
            },
        },
        abort_existing=True,
        dump_tables=True)


if __name__ == "__main__":
    main()

