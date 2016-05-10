#!/usr/bin/env python

import yt.logger as logger
import yt.wrapper as yt

from datetime import datetime, timedelta

import os
import argparse
import logging

logger.set_formatter(logging.Formatter('%(asctime)-15s\t{}\t%(message)s'.format(yt.config.http.PROXY)))

def get_time(obj):
    return obj.attributes["modification_time"]

def get_age(obj):
    "2012-10-19T11:22:58.190448Z"
    pattern = "%Y-%m-%dT%H:%M:%S"

    time_str = get_time(obj)
    time_str = time_str.rsplit(".")[0]
    return datetime.utcnow() - datetime.strptime(time_str, pattern)

def is_locked(obj):
    return any(map(lambda l: l["mode"] in ["exclusive", "shared"], obj.attributes["locks"]))

def main():
    parser = argparse.ArgumentParser(description="Clean operations from cypress.")
    parser.add_argument("--directory", default="//tmp")
    parser.add_argument("--account", default="tmp")
    parser.add_argument("--max-disk-space", type=int, default=None)
    parser.add_argument("--max-chunk-count", type=int, default=None)
    parser.add_argument("--max-node-count", type=int, default=50000)
    parser.add_argument("--safe-age", type=int, default=10, help="Objects that younger than safe-age minutes will not be removed")
    parser.add_argument("--max-age", type=int, default=7, help="Objects that older than max-age days will be removed")
    args = parser.parse_args()

    safe_age = timedelta(minutes=args.safe_age)
    max_age = timedelta(days=args.max_age)

    if args.max_disk_space is None:
        args.max_disk_space = yt.get("//sys/accounts/{0}/@resource_limits/disk_space".format(args.account)) / 2

    if args.max_chunk_count is None:
        args.max_chunk_count = yt.get("//sys/accounts/{0}/@resource_limits/chunk_count".format(args.account)) / 2

    if args.max_node_count is None:
        args.max_node_count = yt.get("//sys/accounts/{0}/@resource_limits/node_count".format(args.account)) / 2

    # collect links
    links = list(yt.search(args.directory, node_type="link"))

    # collect dirs
    dirs = list(yt.search(args.directory, object_filter=lambda obj: obj.attributes["type"] in ["map_node", "list_node"], attributes=["modification_time", "count", "type"]))
    dir_sizes = dict((str(obj), obj.attributes["count"]) for obj in dirs)

    # collect table and files
    objects = []
    for obj in yt.search(args.directory,
                         node_type=["table", "file", "link"],
                         attributes=["access_time", "modification_time", "locks", "hash", "resource_usage"]):
        if is_locked(obj):
            continue
        objects.append((get_age(obj), obj))
    objects.sort()

    to_remove = []
    to_remove_set = set()
    def add_to_remove(obj):
        obj = str(obj)
        if obj not in to_remove_set:
            to_remove.append(obj)
            to_remove_set.add(obj)

    disk_space = 0
    node_count = 0
    chunk_count = 0
    for age, obj in objects:
        node_count += 1
        disk_space += int(obj.attributes["resource_usage"]["disk_space"])
        # filter object by age, total size and count
        if (age > max_age or disk_space > args.max_disk_space or node_count > args.max_node_count or chunk_count > args.max_chunk_count) and age > safe_age:
            if "hash" in obj.attributes:
                link = os.path.join(os.path.dirname(obj), "hash", obj.attributes["hash"])
                if link in links:
                    add_to_remove(link)
            add_to_remove(obj)

    # log and remove
    for obj in to_remove:
        try:
            new_obj_info = yt.get(obj, attributes=["modification_time", "access_time"])
        except yt.YtResponseError as error:
            if not error.is_resolve_error():
                raise
        if get_age(new_obj_info) <= safe_age:
            continue
        info = ""
        if hasattr(obj, "attributes"):
            info = "(size=%s) (access_time=%s)" % (obj.attributes["resource_usage"]["disk_space"], get_time(obj))
        logger.info("Removing %s %s", obj, info)

        dir_sizes[os.path.dirname(obj)] -= 1
        try:
            yt.remove(obj, force=True)
        except yt.YtResponseError as error:
            if not error.is_concurrent_transaction_lock_conflict():
                raise

    # check broken links
    for obj in yt.search(args.directory, node_type=["link"], attributes=["broken"]):
        if str(obj.attributes["broken"]) == "true":
            logger.warning("Removing broken link %s", obj)
            yt.remove(obj, force=True)

    for iter in xrange(5):
        for dir in dirs:
            if dir_sizes[str(dir)] == 0 and get_age(dir).days > args.max_age:
                logger.info("Removing empty dir %s", dir)
                # To avoid removing twice
                dir_sizes[str(dir)] = -1
                try:
                    yt.remove(dir, force=True)
                    dir_sizes[os.path.dirname(dir)] -= 1
                except yt.YtResponseError as error:
                    if not error.is_concurrent_transaction_lock_conflict():
                        raise
                except yt.YtError:
                    logger.info("Failed to remove dir %s", dir)

if __name__ == "__main__":
    main()

