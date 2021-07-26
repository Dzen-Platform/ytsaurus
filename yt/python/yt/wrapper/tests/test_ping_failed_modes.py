# -*- coding: utf-8 -*-

from __future__ import print_function

from .conftest import authors
from .helpers import set_config_option

import yt.wrapper as yt

import pytest
import sys
import time


get_time = time.monotonic if hasattr(time, 'monotonic') else time.time


def reproduce_transaction_loss(
    must_interrupt_sleep=False,
    delay_time=1.5,
    wait_time=3.0,
    proxy_request_timeout=0.1,
):
    with set_config_option("proxy/request_timeout", int(proxy_request_timeout * 1000)):
        tx_context_manager = yt.YtClient(token=yt.config["token"], config=yt.config.config).Transaction()
        tx = tx_context_manager.__enter__()
        time.sleep(delay_time)

        wait_begin = get_time()
        wait_end = wait_begin + wait_time
        first_exception = None
        first_sleep_duration = wait_time
        aborted = False
        
        while True:
            # Since aborted transaction may cause multiple interruptions,
            # we sleep for wait_time catching all exceptions,
            # and then reraise the first caught one.
            try:
                if not aborted:
                    aborted = True
                    yt.YtClient(token=yt.config["token"], config=yt.config.config).abort_transaction(tx.transaction_id)

                remaining_wait_time = wait_end - get_time()
                if remaining_wait_time <= 0:
                    break
                time.sleep(remaining_wait_time)
            except BaseException as exception:
                if not first_exception:
                    first_exception = exception
                    first_sleep_duration = get_time() - wait_begin
        try:
            tx_context_manager.__exit__(*sys.exc_info())
        except yt.errors.YtNoSuchTransaction:
            pass

        if must_interrupt_sleep:
            assert first_sleep_duration < 0.9 * wait_time, (first_sleep_duration, wait_time)

        if first_exception:
            raise first_exception


@pytest.mark.usefixtures("yt_env_with_rpc")
class TestPingFailedModes(object):
    @authors("marat-khalili")
    def test_invalid_value(self):
        with set_config_option("ping_failed_mode", "invalid_value"):
            with pytest.raises(yt.YtError):
                yt.Transaction()

    @authors("marat-khalili")
    def test_call_function_missing(self):
        with set_config_option("ping_failed_mode", "call_function"):
            with pytest.raises(yt.YtError):
                yt.Transaction()

    @authors("marat-khalili")
    def test_call_function_invalid(self):
        with set_config_option("ping_failed_mode", "call_function"), set_config_option("ping_failed_function", 1):
            with pytest.raises(yt.YtError):
                yt.Transaction()

    @authors("marat-khalili")
    def test_call_function(self):
        called = []
        assert not called
        def func():
            called.append(True)
        with set_config_option("ping_failed_mode", "call_function"), set_config_option("ping_failed_function", func):
            reproduce_transaction_loss()
        assert called

    @authors("marat-khalili")
    def test_interrupt_main(self):
        with set_config_option("ping_failed_mode", "interrupt_main"):
            with pytest.raises(KeyboardInterrupt):
                reproduce_transaction_loss()

    @authors("marat-khalili")
    def test_pass(self):
        with set_config_option("ping_failed_mode", "pass"):
            reproduce_transaction_loss()

    @authors("marat-khalili")
    def test_send_signal(self):
        with set_config_option("ping_failed_mode", "send_signal"):
            with pytest.raises(yt.YtTransactionPingError):
                reproduce_transaction_loss(must_interrupt_sleep=True)
