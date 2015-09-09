#!/usr/bin/env python

import yt.wrapper as yt

import os
import argparse
from datetime import date, timedelta

def assign_list(old, new):
    old[:] = []
    for elem in new:
        old.append(elem)

def process_logs(import_list, remove_list, link_queue, destination_dir, source_pattern, destination_pattern, days, make_link):
    def get_dst(elem):
        if isinstance(elem, dict):
            return elem["dst"]
        return elem

    if days is None:
        count = 15
    else:
        count = days + 15

    if destination_pattern is None:
        destination_pattern = source_pattern

    for i in xrange(count):
        table_date = date.today() - timedelta(days=i)
        def to_name(pattern):
            return pattern.format(table_date.strftime("%Y%m%d"))
        def to_link(pattern):
            return pattern.format(table_date.strftime("%Y-%m-%d"))

        src = to_name(source_pattern)
        destination_path = os.path.join(destination_dir, destination_pattern)
        dst = to_name(destination_path)
        dst_link = to_link(destination_path)
        if days is not None and i >= days: # Remove case
            if yt.exists(dst) and dst not in remove_list:
                if make_link:
                    remove_list.append(dst_link)
                remove_list.append(dst)
            assign_list(link_queue, [elem for elem in link_queue if elem["src"] != dst])
            assign_list(import_list, [elem for elem in import_list if get_dst(elem) != dst])
        else: # Import case
            if dst in map(get_dst, import_list):
                continue
            if not yt.exists(dst) or yt.get(dst + "/@row_count") == 0:
                import_list.append({"src": src, "dst": dst, "mr_user": "userdata"})
            if make_link and not yt.exists(dst_link) and not dst in map(get_dst, link_queue):
                link_queue.append({"src": dst, "dst": dst_link})

def main():
    parser = argparse.ArgumentParser(description='Prepare tables to merge')
    parser.add_argument('--path', required=True)
    parser.add_argument('--import-queue', required=True)
    parser.add_argument('--remove-queue', required=True)
    parser.add_argument('--link-queue', required=True)
    parser.add_argument('--user-sessions-period', type=int, default=50)
    args = parser.parse_args()

    tables_to_import = yt.get(args.import_queue)
    tables_to_remove = yt.get(args.remove_queue)
    link_queue = yt.get(args.link_queue)

    def process(source, destination, days, link):
        process_logs(tables_to_import, tables_to_remove, link_queue, args.path, source, destination, days, link)

    process("tr/user_sessions/{}",          None,                        args.user_sessions_period,         True)

    yt.set(args.import_queue, list(tables_to_import))
    yt.set(args.remove_queue, list(tables_to_remove))
    yt.set(args.link_queue, link_queue)

if __name__ == "__main__":
    main()
