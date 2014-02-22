import config
import yt.logger as logger
from common import YtError, get_backoff
from errors import YtNetworkError 
from table import to_table
from transaction import PingableTransaction
from transaction_commands import _make_transactional_request
from driver import get_host_for_heavy_operation

import time
from datetime import datetime

def make_heavy_request(command_name, stream, path, params, create_object, use_retries):
    path = to_table(path)

    title = "Python wrapper: {0} {1}".format(command_name, path.name)
    with PingableTransaction(timeout=config.http.REQUEST_TIMEOUT,
                             attributes={"title": title}):
        create_object(path.name)
        if use_retries:
            started = False
            for chunk in stream:
                if started:
                    path.append = True
                started = True

                logger.debug("Processing {0} chunk (length: {1}, transaction: {2})"
                    .format(command_name, len(chunk), config.TRANSACTION))
                
                for attempt in xrange(config.http.REQUEST_RETRY_COUNT):
                    current_time = datetime.now()
                    try: 
                        with PingableTransaction(timeout=config.http.REQUEST_TIMEOUT):
                            params["path"] = path.get_json()
                            _make_transactional_request(
                                command_name,
                                params,
                                data=chunk,
                                proxy=get_host_for_heavy_operation(),
                                retry_unavailable_proxy=False)
                        break
                    except (YtNetworkError, YtError) as err:
                        if attempt + 1 == config.http.REQUEST_RETRY_COUNT:
                            raise
                        backoff = get_backoff(config.http.REQUEST_RETRY_TIMEOUT, current_time)
                        if backoff:
                            logger.warning("%s. Sleep for %.2lf seconds...", err.message, backoff)
                            time.sleep(backoff)
                        logger.warning("New retry (%d) ...", attempt + 2)
        else:
            params["path"] = path.get_json()
            _make_transactional_request(
                command_name,
                params,
                data=stream,
                proxy=get_host_for_heavy_operation())
