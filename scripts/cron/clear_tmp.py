#!/usr/bin/env python

import yt.logger as logger
import yt.wrapper as yt

from datetime import datetime, timedelta

import os
import argparse
import logging

logger.set_formatter(logging.Formatter('%(asctime)-15s\t{}\t%(message)s'.format(yt.config["proxy"]["url"])))

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
    parser.add_argument("--account")
    parser.add_argument("--max-disk-space", type=int, default=None)
    parser.add_argument("--max-chunk-count", type=int, default=None)
    parser.add_argument("--max-node-count", type=int, default=50000)
    parser.add_argument("--max-dir-node-count", type=int, default=40000)
    parser.add_argument("--safe-age", type=int, default=10, help="Objects that younger than safe-age minutes will not be removed")
    parser.add_argument("--max-age", type=int, default=7, help="Objects that older than max-age days will be removed")
    parser.add_argument("--do-not-remove-objects-with-other-account", action="store_true", default=False, help="By default all objects in directory will be removed")
    parser.add_argument("--do-not-remove-objects-with-locks", action="store_true", default=False, help="Do not remove objects with any locks on them")
    args = parser.parse_args()

    safe_age = timedelta(minutes=args.safe_age)
    max_age = timedelta(days=args.max_age)

    if args.max_disk_space is None and args.account is not None:
        args.max_disk_space = yt.get("//sys/accounts/{0}/@resource_limits/disk_space".format(args.account)) / 2

    if args.max_chunk_count is None and args.account is not None:
        args.max_chunk_count = yt.get("//sys/accounts/{0}/@resource_limits/chunk_count".format(args.account)) / 2

    if args.max_node_count is None and args.account is not None:
        args.max_node_count = yt.get("//sys/accounts/{0}/@resource_limits/node_count".format(args.account)) / 2

    if not yt.exists(args.directory):
        return

    # collect aux objects
    dirs = []
    links = set()
    aux_objects = yt.search(args.directory,
                            object_filter=lambda obj: obj.attributes["type"] in ["link", "map_node", "list_node"],
                            attributes=["modification_time", "count", "type", "account"])
    for obj in aux_objects:
        if obj.attributes["type"] == "link":
            links.add(str(obj))
        else: #dir case
            dirs.append(obj)
    dir_sizes = dict((str(obj), obj.attributes["count"]) for obj in dirs)
    remaining_dir_sizes = dict((str(obj), obj.attributes["count"]) for obj in dirs)

    object_to_attributes = {}

    # collect table and files
    objects = []
    for obj in yt.search(args.directory,
                         node_type=["table", "file", "link"],
                         attributes=["access_time", "modification_time", "locks", "hash", "resource_usage", "account", "type", "target_path"]):
        if is_locked(obj):
            continue
        if args.do_not_remove_objects_with_other_account and obj.attributes.get("account") != args.account:
            continue
        object_to_attributes[str(obj)] = obj.attributes
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
        # filter by locks
        if args.do_not_remove_objects_with_locks:
            if obj.attributes["locks"]:
                continue
            if obj.attributes["type"] == "link":
                target_path = obj.attributes["target_path"]
                if target_path in object_to_attributes and object_to_attributes[target_path]["locks"]:
                    continue

        dir_node_count = remaining_dir_sizes.get(os.path.dirname(obj), 0)

        # filter object by age, total size and count
        node_count_violated = args.max_node_count is not None and node_count > args.max_node_count
        disk_space_violated = args.max_disk_space is not None and disk_space > args.max_disk_space
        chunk_count_violated = args.max_chunk_count is not None and chunk_count > args.max_chunk_count
        if (age > max_age or disk_space_violated or node_count_violated or chunk_count_violated or dir_node_count > args.max_dir_node_count) and age > safe_age:
            if "hash" in obj.attributes:
                hash_str = obj.attributes["hash"]
                link = os.path.join(os.path.dirname(obj), "hash", hash_str[-2:], hash_str)
                if link in links:
                    add_to_remove(link)
                link = os.path.join(os.path.dirname(obj), "hash", hash_str)
                if link in links:
                    add_to_remove(link)
            if dir_node_count > 0:
                remaining_dir_sizes[os.path.dirname(obj)] -= 1
            add_to_remove(obj)

    # log and remove
    for obj in to_remove:
        try:
            new_obj_info = yt.get(obj, attributes=["modification_time", "access_time"])
        except yt.YtResponseError as error:
            if not error.is_resolve_error():
                raise
            continue
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
    for obj in yt.search(args.directory, node_type=["link"], attributes=["broken", "account"]):
        if args.do_not_remove_objects_with_other_account and obj.attributes.get("account") != args.account:
            continue
        if str(obj.attributes["broken"]) == "true":
            logger.warning("Removing broken link %s", obj)
            yt.remove(obj, force=True)

    for iter in xrange(5):
        for dir in dirs:
            if args.do_not_remove_objects_with_other_account and dir.attributes.get("account") != args.account:
                continue
            if dir_sizes[str(dir)] == 0 and get_age(dir).days > args.max_age:
                logger.info("Removing empty dir %s", dir)
                # To avoid removing twice
                dir_sizes[str(dir)] = -1
                try:
                    try:
                        yt.remove(dir, force=True)
                        dir_sizes[os.path.dirname(dir)] -= 1
                    except yt.YtResponseError as error:
                        if not error.is_concurrent_transaction_lock_conflict():
                            raise
                except yt.YtError:
                    logger.exception("Failed to remove dir %s", dir)

if __name__ == "__main__":
    main()

