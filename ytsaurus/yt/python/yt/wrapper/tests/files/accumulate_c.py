#!/usr/bin/env python

from __future__ import print_function

import sys
from functools import partial

try:
    from itertools import imap
except ImportError:  # Python 3
    imap = map

def capitalizeB(rec):
    if "b" in rec: rec["b"] = rec["b"].upper()
    return rec

""" Methods for records conversion """
def record_to_line(rec, eoln=True):
    body = "\t".join("=".join(imap(str, item)) for item in rec.items())
    return "%s%s" % (body, "\n" if eoln else "")

def line_to_record(line):
    return dict(field.split("=", 1) for field in line.strip("\n").split("\t"))

if __name__ == "__main__":
    lines = sys.stdin.readlines()
    print(lines, file=sys.stderr)
    recs = list(imap(partial(line_to_record), lines))
    print(recs, file=sys.stderr)

    res = []

    curA = None
    sum = 0
    for rec in recs:
        if "a" not in rec: continue
        a = rec["a"]
        if a != curA:
            if curA is not None:
                res.append({"a": curA, "c": sum})
            curA = a
            sum = 0.0
        sum += float(rec.get("c", 0.0))

    if curA is not None:
        res.append({"a": curA, "c": sum})

    sys.stdout.writelines(imap(partial(record_to_line), res))
