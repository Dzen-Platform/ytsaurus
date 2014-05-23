""" Old style mapreduce records.
    Copy-pasted from mapreducelib.py with some additions.
"""

class SimpleRecord:
    """Mapreduce-like record represents (key, value) pair, without subkey"""
    def __init__(self, key, value, tableIndex=0):
        """:param tableIndex: """
        self.key = key
        self.value = value
        self.tableIndex = tableIndex

    def __str__(self):
        return "Record('%s', '%s')" % (self.key, self.value)

    def __repr__(self):
        return self.__str__()

    def __cmp__(self, other):
        cmps = [cmp(getattr(self, field), getattr(other, field))
                for field in ["key", "value"]]
        non_zeroes = filter(None, cmps) + [0]
        return non_zeroes[0]

    def __hash__(self):
        return hash(frozenset([self.key, self.value]))

    def items(self):
        return self.key, self.value

    def getTableIndex(self):
        return self.tableIndex

class SubkeyedRecord(SimpleRecord):
    """Mapreduce-like record with key, subkey and value"""
    def __init__(self, key, subkey, value, tableIndex=0):
        SimpleRecord.__init__(self, key, value, tableIndex)
        self.subkey = subkey

    def items(self):
        return self.key, self.subkey, self.value

    def getTableIndex(self):
        return self.tableIndex

    def __str__(self):
        return "Record('%s', '%s', '%s')" % (self.key, self.subkey, self.value)

    def __repr__(self):
        return self.__str__()

    def __cmp__(self, other):
        cmps = [cmp(getattr(self, field), getattr(other, field))
                for field in ["key", "subkey", "value"]]
        non_zeroes = filter(None, cmps) + [0]
        return non_zeroes[0]

    def __hash__(self):
        return hash(frozenset([self.key, self.subkey, self.value]))

def Record(*args, **kws):
    """Return mapreduce-like record with key, subkey, value."""
    assert len(args) >= 2, "incorrect arguments count [ARGS: %s]" % repr(args)
    if len(args) < 3:
        return SimpleRecord(*args, **kws)
    return SubkeyedRecord(*args[:3], **kws)
