# -*- coding: utf-8 -*-

import getpass

import yt.wrapper

#
# Для того чтобы запустить операцию mapreduce, нам нужны обычные маппер и редьюсер
# (их даже можно использовать в других местах в отдельных операциях map/reduce).
#

def normalize_name_mapper(row):
    normalized_name = row["name"].lower()
    yield {"name": normalized_name}

def count_names_reducer(key, input_row_iterator):
    name = key["name"]

    count = 0
    for input_row in input_row_iterator:
        count += 1

    yield {"name": name, "count": count}

if __name__ == "__main__":
    yt.wrapper.config.set_proxy("freud")

    output_table = "//tmp/" + getpass.getuser() + "-pytutorial-name-stat"

    # Запуск операции MapReduce несильно отличается от запуска других операций.
    # Нам надо указать список ключей, по которым мы будем редьюсить,
    # а так же маппер и редьюсер.
    yt.wrapper.run_map_reduce(
        normalize_name_mapper,
        count_names_reducer,
        source_table="//home/ermolovd/yt-tutorial/staff_unsorted",
        destination_table=output_table,
        reduce_by=["name"])

    print("Output table: https://yt.yandex-team.ru/freud/#page=navigation&offsetMode=row&path={0}".format(output_table))
