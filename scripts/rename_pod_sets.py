import argparse
import sys

from yp.local import TablePreparer, get_db_version, backup_yp
from yt.wrapper import YtClient

from yp_proto.yp.client.api.proto.object_type_pb2 import EObjectType

def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--yt-proxy", required=True, help="YT cluster.")
    parser.add_argument("--yp-path", required=True, help="YP path.")
    parser.add_argument("--commit", action="store_true", help="Change DB if given.")
    return parser.parse_args(argv)

def modify_pod_set_id(row, pod_set_key, pod_sets_mapping):
    if row[pod_set_key] in pod_sets_mapping:
        row[pod_set_key] = pod_sets_mapping[row[pod_set_key]]
    return row

def main(argv):
    args = parse_args(argv)

    yt_client = YtClient(args.yt_proxy)
    if args.commit:
        backup_yp(yt_client, args.yp_path)

    table_preparer = TablePreparer(yt_client, args.yp_path, version=get_db_version(yt_client, args.yp_path))

    pod_sets_mapping = {}
    for row in yt_client.select_rows("[meta.id], [labels] from [{}]".format(table_preparer.get_table_path("pod_sets"))):
        if row.get("labels", {}).get("deploy_engine", "") != "YP_LITE":
            continue
        if row["meta.id"] == row["labels"]["nanny_service_id"]:
            continue
        pod_sets_mapping[row["meta.id"]] = row["labels"]["nanny_service_id"]

    for pod_set_id, nanny_service_id in pod_sets_mapping.iteritems():
        print "pod_set {} will be renamed to {}".format(pod_set_id, nanny_service_id)

    if not args.commit:
        return

    def modify_pod_sets(row):
        yield modify_pod_set_id(row, "meta.id", pod_sets_mapping)
    table_preparer.run_map("pod_sets", modify_pod_sets)
    
    def modify_pods(row):
        yield modify_pod_set_id(row, "meta.pod_set_id", pod_sets_mapping)
    table_preparer.run_map("pods", modify_pods)

    def modify_node_segment_to_pod_sets(row):
        yield modify_pod_set_id(row, "pod_set_id", pod_sets_mapping)
    table_preparer.run_map("node_segment_to_pod_sets", modify_node_segment_to_pod_sets)

    def modify_account_to_pod_sets(row):
        yield modify_pod_set_id(row, "pod_set_id", pod_sets_mapping)
    table_preparer.run_map("account_to_pod_sets", modify_account_to_pod_sets)

    def modify_parents(row):
        if row["object_type"] == EObjectType.Value("OT_POD"):
            yield modify_pod_set_id(row, "parent_id", pod_sets_mapping)
        else:
            yield row

    table_preparer.finalize()

if __name__ == "__main__":
    main(sys.argv[1:])
