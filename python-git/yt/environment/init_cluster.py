#!/usr/bin/env python

import yt.wrapper as yt
import yt.logger as logger
from yt.common import get_value

import string
import argparse

TB = 1024 ** 4

def create(type_, name, client):
    try:
        client.create(type_, attributes={"name": name})
    except yt.YtResponseError as err:
        if err.contains_code(501):
            logger.warning("'%s' already exists", name)
        else:
            raise

def add_member(subject, group, client):
    try:
        client.add_member(subject, group)
    except yt.YtResponseError as err:
        if "is already present in group" in err.message:
            logger.warning(err.message)
        else:
            raise

def check_acl(acl, required_keys, optional_keys):
    for k in required_keys:
        if k not in acl:
            logger.warning("Can't find required key '%s' in ACL: %s", k, acl)
            return False
    for k in acl:
        if k not in optional_keys and k not in required_keys:
            logger.warning("Found unknown key '%s' in ACL: %s", k, acl)
            return False
    return True

def need_to_add_new_acl(new_acl, current_acls):
    required_keys = ["subjects", "permissions", "action"]
    optional_keys = ["inheritance_mode"]

    if not check_acl(new_acl, required_keys, optional_keys):
        return False

    for cur_acl in current_acls:
        found_equal_acl = True
        for k in new_acl:
            if k in cur_acl:
                if sorted(new_acl[k]) != sorted(cur_acl[k]):
                    found_equal_acl = False

        if found_equal_acl:
            return False

    return True

def add_acl(path, new_acl, client):
    current_acls = client.get(path + "/@acl")

    if need_to_add_new_acl(new_acl, current_acls):
        client.set(path + "/@acl/end", new_acl)
    else:
        logger.warning("ACL '%s' is already present in %s/@acl", new_acl, path)

        # Backwards compatibility.

def get_default_resource_limits(client):
    result = {"node_count": 200000, "chunk_count": 1000000}
    if client.exists("//sys/media"):
        result["disk_space_per_medium"] = {"default": 10 * TB}
    else:
        result["disk_space"] = 10 * TB

    return result

def initialize_world(client=None, idm=None, proxy_address=None, ui_address=None):
    client = get_value(client, yt)
    users = ["odin", "cron", "nightly_tester", "application_operations"]
    groups = ["devs", "admins"]
    if idm:
        groups.append("yandex")
    everyone_group = "everyone" if not idm else "yandex"

    for user in users:
        create("user", user, client)
    for group in groups:
        create("group", group, client)
    add_member("cron", "superusers", client)
    add_member("devs", "admins", client)
    if idm:
        add_member("users", "yandex", client)
    add_member("application_operations", "superusers", client)

    for dir in ["//sys", "//tmp", "//sys/tokens"]:
        client.set(dir + "/@opaque", "true")

    add_acl("/", {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]}, client)
    add_acl("/", {"action": "allow", "subjects": ["admins"], "permissions": ["write", "remove", "administer"]}, client)

    add_acl("//sys", {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]}, client)
    add_acl("//sys", {"action": "allow", "subjects": ["admins"], "permissions": ["write", "remove", "administer"]},
            client)
    client.set("//sys/@inherit_acl", "false")

    add_acl("//sys/accounts/sys", {"action": "allow", "subjects": ["root", "admins"], "permissions": ["use"]}, client)

    add_acl("//sys/tokens", {"action": "allow", "subjects": ["admins"], "permissions": ["read", "write", "remove"]},
            client)
    client.set("//sys/tokens/@inherit_acl", "false")

    if not client.exists("//home"):
        client.create("map_node", "//home",
                      attributes={
                          "opaque": "true",
                          "account": "tmp"})
    # add_acl to schemas
    for schema in ["user", "group", "tablet_cell"]:
        if client.exists("//sys/schemas/%s" % schema):
            client.set("//sys/schemas/%s/@acl" % schema,
                       [
                           {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]},
                           {"action": "allow", "subjects": ["admins"], "permissions": ["write", "remove", "create"]}
                       ])

    if client.exists("//sys/schemas/account"):
        client.set("//sys/schemas/account/@acl",
                   [
                       {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]},
                       {"action": "allow", "subjects": ["admins"],
                        "permissions": ["write", "remove", "create", "administer", "use"]}
                   ])

    for schema in ["rack", "cluster_node", "data_center"]:
        if client.exists("//sys/schemas/%s" % schema):
            client.set("//sys/schemas/%s/@acl" % schema,
                       [
                           {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]},
                           {"action": "allow", "subjects": ["admins"],
                            "permissions": ["write", "remove", "create", "administer"]}
                       ])
    if client.exists("//sys/schemas/lock"):
        client.set("//sys/schemas/lock/@acl",
                   [
                       {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]},
                   ])

    if client.exists("//sys/schemas/transaction"):
        client.set("//sys/schemas/transaction/@acl",
               [
                   {"action": "allow", "subjects": [everyone_group], "permissions": ["read"]},
                   {"action": "allow", "subjects": ["users"], "permissions": ["write", "create"]}
               ])

    if not client.exists("//sys/empty_yamr_table"):
        yamr_table_schema = [{"name": name, "type": "any", "sort_order": "ascending"}
                             for name in ["key", "subkey"]] + [{"name": "value", "type": "any"}]
        client.create("table", "//sys/empty_yamr_table", attributes={"schema": yamr_table_schema})

    if not client.exists("//sys/accounts/tmp_files"):
        client.create("account", attributes={"name": "tmp_files",
                                             "acl": [{
                                                 "action": "allow",
                                                 "subjects": ["users"],
                                                 "permissions": ["use"]
                                             }],
                                             "resource_limits": get_default_resource_limits(client)})
    else:
        logger.warning("Account 'tmp_files' already exists")

    client.create("//tmp/yt_wrapper/file_storage",
                  attributes={"account": "tmp_files"},
                  recursive=True,
                  ignore_existing=True)

    if not client.exists("//sys/tablet_cell_bundles/sys"):
        client.create("tablet_cell_bundle", attributes={"name": "sys"})
    else:
        logger.warning("Tablet cell bundle \"sys\" already exists")

    if proxy_address is not None:
        client.set("//sys/@cluster_proxy_address", proxy_address)
    if ui_address is not None:
        client.set("//sys/@cluster_ui_address", ui_address)


def main():
    parser = argparse.ArgumentParser(description="new YT cluster init script")
    parser.add_argument("--idm", action="store_true", dest="idm", default=False,
                        help="Use IDM system with this cluster")
    args = parser.parse_args()

    proxy_address = None
    ui_address = None
    if yt.config["proxy"]["url"]:
        suffix = yt.config["proxy"]["default_suffix"]
        proxy_short_address = yt.config["proxy"]["url"]
        if proxy_short_address.endswith(suffix):
            proxy_short_address = proxy_short_address[:-len(suffix)]
        if all(ch in string.ascii_letters + string.digits for ch in proxy_short_address):
            proxy_address = proxy_short_address + suffix
            ui_address = "https://yt.yandex-team.ru/{0}/".format(proxy_short_address)

    initialize_world(idm=args.idm, proxy_address=proxy_address, ui_address=ui_address)

if __name__ == "__main__":
    main()
