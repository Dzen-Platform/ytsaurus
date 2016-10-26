from .config import get_config, get_option
from .driver import make_request, make_formatted_request
from .common import update, bool_to_string, get_value, get_started_by

from copy import deepcopy

def transaction_params(transaction=None, ping_ancestor_transactions=None, client=None):
    transaction = get_value(transaction, get_option("TRANSACTION", client))
    ping_ancestor_transactions = get_value(ping_ancestor_transactions, get_option("PING_ANCESTOR_TRANSACTIONS", client))
    return {"ping_ancestor_transactions": bool_to_string(ping_ancestor_transactions),
            "transaction_id": transaction}

def _add_transaction_params(params, client=None):
    return update(deepcopy(params), transaction_params(client=client))

def _make_transactional_request(command_name, params, **kwargs):
    return make_request(command_name, _add_transaction_params(params, kwargs.get("client", None)), **kwargs)

def _make_formatted_transactional_request(command_name, params, format, **kwargs):
    return make_formatted_request(command_name, _add_transaction_params(params, kwargs.get("client", None)), format, **kwargs)

def start_transaction(parent_transaction=None, timeout=None, attributes=None, type="master", sticky=False, client=None):
    """Start transaction.

    :param parent_transaction: (string) parent transaction id
    :param timeout: transaction lifetime singe last ping in milliseconds
    :param type: could be either "master" or "tablet"
    :param sticky: EXPERIMENTAL, do not use it, unless you have been told to do so
    :param attributes: (dict)
    :return: (string) new transaction id
    .. seealso:: `start_tx on wiki <https://wiki.yandex-team.ru/yt/userdoc/api#starttx>`_
    """
    params = transaction_params(parent_transaction, client=client)
    timeout = get_value(timeout, get_config(client)["transaction_timeout"])
    if timeout is not None:
        params["timeout"] = int(timeout)
    params["attributes"] = update(get_started_by(), get_value(attributes, {}))
    params["type"] = type
    params["sticky"] = bool_to_string(sticky)
    return make_formatted_request("start_tx", params, None, client=client)

def abort_transaction(transaction, sticky=False, client=None):
    """Abort transaction. All changes will be lost.

    :param transaction: (string) transaction id
    .. seealso:: `abort_tx on wiki <https://wiki.yandex-team.ru/yt/userdoc/api#aborttx>`_
    """
    params = transaction_params(transaction, client=client)
    params["sticky"] = sticky
    make_request("abort_tx", params, client=client)

def commit_transaction(transaction, sticky=False, client=None):
    """Save all transaction changes.

    :param transaction: (string) transaction id
    .. seealso:: `commit_tx on wiki <https://wiki.yandex-team.ru/yt/userdoc/api#committx>`_
    """
    params = transaction_params(transaction, client=client)
    params["sticky"] = sticky
    make_request("commit_tx", params, client=client)

def ping_transaction(transaction, sticky=False, client=None):
    """Prolong transaction lifetime.

    :param transaction: (string) transaction id
    .. seealso:: `ping_tx on wiki <https://wiki.yandex-team.ru/yt/userdoc/api#pingtx>`_
    """
    params = transaction_params(transaction, client=client)
    params["sticky"] = sticky
    make_request("ping_tx", params, client=client)
