#!/usr/bin/env python

import os
import sys

if __name__ == "__main__":
    for line in sys.stdin:
        pass

    os.write(1, "q\tw\te\n")
    os.write(4, "4\t4\t4\n")

