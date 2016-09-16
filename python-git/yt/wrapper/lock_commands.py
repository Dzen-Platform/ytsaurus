from .common import bool_to_string, get_value, YtError
from .table import prepare_path
from .cypress_commands import get
from .transaction_commands import _make_formatted_transactional_request

import time
from datetime import timedelta, datetime

def lock(path, mode=None, waitable=False, wait_for=None, child_key=None, attribute_key=None, client=None):
    """Try to lock the path.

    :param mode: (optional) blocking type ["snapshot", "shared" or "exclusive" (default)]
    :param waitable: (bool) wait for lock if node is under blocking
    :param wait_for: (int) wait interval in milliseconds. If timeout occurred, `YtError` raised
    :return: taken lock id (YSON string) or throws YtHttpResponseError with 40* code if lock conflict detected.

    .. seealso:: `lock on wiki <https://wiki.yandex-team.ru/yt/userdoc/transactions#versionirovanieiloki>`_
    """
    if wait_for is not None:
        wait_for = timedelta(milliseconds=wait_for)

    params = {
        "path": prepare_path(path, client=client),
        "mode": get_value(mode, "exclusive"),
        "waitable": bool_to_string(waitable)}

    if child_key is not None:
        params["child_key"] = child_key
    if attribute_key is not None:
        params["attribute_key"] = attribute_key

    lock_id = _make_formatted_transactional_request("lock", params, format=None, client=client)
    if not lock_id:
        return None

    if waitable and wait_for is not None and lock_id != "0-0-0-0":
        now = datetime.now()
        acquired = False
        while datetime.now() - now < wait_for:
            if get("#%s/@state" % lock_id, client=client) == "acquired":
                acquired = True
                break
            time.sleep(1.0)
        if not acquired:
            raise YtError("Timed out while waiting {0} milliseconds for lock {1}".format(wait_for.microseconds // 1000 + wait_for.seconds * 1000, lock_id))

    return lock_id
