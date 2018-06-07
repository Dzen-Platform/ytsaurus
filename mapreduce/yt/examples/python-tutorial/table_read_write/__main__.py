# -*- coding: utf-8 -*-

import os

import yt.wrapper

if __name__ == "__main__":
    yt.wrapper.config.set_proxy("freud")

    table = "//tmp/" + os.getlogin() + "-read-write"

    # Просто пишем данные в таблицу, если таблица существует, её перезапишут.
    yt.wrapper.write_table(table, [
        {"english": "one", "russian": "один"},
        {"english": "two", "russian": "два"},
    ])

    # Дописываем данные в конец таблицы, придёся поступить хитрее.
    # Используем класс TablePath и его опцию append.
    yt.wrapper.write_table(yt.wrapper.TablePath(table, append=True), [
        {"english": "three", "russian": "три"},
    ])

    # Читаем всю таблицу.
    print "*** ALL TABLE ***"
    for row in yt.wrapper.read_table(table):
        print "english:", row["english"], "; russian:", row["russian"]
    print "*****************"
    print ""

    # Читаем первые 2 строки таблицы.
    print "*** FIRST TWO ROWS ***"
    for row in yt.wrapper.read_table(
        yt.wrapper.TablePath(
            table,
            start_index=0, end_index=2) # читаем с 0й по 2ю строки, 2я строка невключительно
    ):
        print "english:", row["english"], "; russian:", row["russian"]
    print "*****************"
    print ""

    #  Если мы отсортируем таблицу, то можно будет читать записи по ключам.
    yt.wrapper.run_sort(table, sort_by=["english"])

    # И читаем запись по одному ключу.
    print "*** EXACT KEY ***"
    for row in yt.wrapper.read_table(
        yt.wrapper.TablePath(
            table,
            exact_key=["three"] # В качестве ключа передаём список значений ключевых колонок
                                # (тех колонок по которым отсортирована таблица).
                                # Тут у нас простой случай, одна ключевая колонка, но их может быть больше.
        )
    ):
        print "english:", row["english"], "; russian:", row["russian"]
    print "*****************"
    print ""
