#!/usr/bin/env python

from __future__ import print_function

import sys
from itertools import imap
from functools import partial

def capitalizeB(rec):
    if "b" in rec:
        rec["b"] = rec["b"].upper()
    return rec

""" Methods for records convertion """
def record_to_line(rec, eoln=True):
    body = "\t".join("=".join(map(str, item)) for item in rec.items())
    return "%s%s" % (body, "\n" if eoln else "")

def line_to_record(line):
    return dict(field.split("=", 1) for field in line.strip("\n").split("\t"))

if __name__ == "__main__":
    lines = sys.stdin.readlines()
    print(lines, file=sys.stderr)
    recs = map(partial(line_to_record), lines)
    print(recs, file=sys.stderr)
    sys.stdout.writelines(imap(partial(record_to_line), imap(capitalizeB, recs)))
