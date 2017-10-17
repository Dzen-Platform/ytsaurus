cimport cython
from libc.stdlib cimport malloc, free
from util.generic.vector cimport yvector
from util.generic.hash cimport yhash
from util.generic.string cimport TString
from util.system.types cimport i64, ui64, ui32


cdef extern from "mapreduce/yt/interface/init.h" namespace "NYT":
    cdef cppclass TInitializeOptions:
        TInitializeOptions() except +
    void Initialize(int, const char**, const TInitializeOptions&) except +


def initialize(*args):
    cdef const char **argv = <const char**>malloc(len(args) * sizeof(char*))
    for i in xrange(len(args)):
        argv[i] = <char*>args[i]
    try:
        Initialize(len(args), argv, TInitializeOptions())
    finally:
        free(argv)


cdef extern from "mapreduce/yt/node/node.h" namespace "NYT" nogil:
    cdef cppclass TNode:
        TNode() except +
        TNode(const char*) except +
        TNode(TString) except +
        TNode(double) except +
        TNode(bint) except +
        TNode(i64) except +
        TNode(ui64) except +

        bint IsString()
        bint IsInt64()
        bint IsUint64()
        bint IsDouble()
        bint IsBool()
        bint IsList()
        bint IsMap()
        bint IsEntity()
        bint IsUndefined()

        TString& AsString()
        i64 AsInt64()
        ui64 AsUint64()
        double AsDouble()
        bint AsBool()
        yvector[TNode]& AsList()
        yhash[TString, TNode]& AsMap()

        @staticmethod
        TNode CreateList()
        @staticmethod
        TNode CreateMap()

        TNode operator()(TString, TNode)
        TNode Add(TNode)


class Node(object):
    INT64 = 0
    UINT64 = 1
    _ALL_TYPES = {INT64, UINT64}

    def __init__(self, data, node_type):
        self.data = data
        if node_type not in Node._ALL_TYPES:
            raise Exception('unsupported node_type')
        self.node_type = node_type


def node_i64(i):
    return Node(i, Node.INT64)


def node_ui64(ui):
    return Node(ui, Node.UINT64)


cdef TString _to_TString(s):
    assert isinstance(s, basestring)
    if isinstance(s, unicode):
        s = s.encode('UTF-8')
    return TString(<const char*>s, len(s))


cdef _TNode_to_pyobj(TNode node):
    if node.IsString():
        return node.AsString()
    elif node.IsInt64():
        return node.AsInt64()
    elif node.IsUint64():
        return node.AsUint64()
    elif node.IsDouble():
        return node.AsDouble()
    elif node.IsBool():
        return node.AsBool()
    elif node.IsEntity():
        return None
    elif node.IsUndefined():
        return None
    elif node.IsList():
        node_list = node.AsList()
        return [_TNode_to_pyobj(n) for n in node_list]
    elif node.IsMap():
        node_map = node.AsMap()
        return {p.first: _TNode_to_pyobj(p.second) for p in node_map}
    else:
        # should never happen
        raise Exception()


cdef TNode _pyobj_to_TNode(obj):
    if isinstance(obj, Node):
        if obj.node_type == Node.INT64:
            return TNode(<i64>obj.data)
        elif obj.node_type == Node.UINT64:
            return TNode(<ui64>obj.data)
        else:
            # should never happen
            raise Exception()
    elif isinstance(obj, basestring):
        return TNode(_to_TString(obj))
    elif isinstance(obj, bool):
        return TNode(<bint>obj)
    elif isinstance(obj, int):
        return TNode(<i64>obj)
    elif isinstance(obj, float):
        return TNode(<float>obj)
    elif isinstance(obj, dict):
        node = TNode.CreateMap()
        for k, v in obj.iteritems():
            node(_to_TString(k), _pyobj_to_TNode(v))
        return node
    elif isinstance(obj, list):
        node = TNode.CreateList()
        for x in obj:
            node.Add(_pyobj_to_TNode(x))
        return node
    elif obj is None:
        return TNode()
    else:
        raise Exception('Can\'t convert {} object to TNode'.format(type(obj)))


cdef extern from "mapreduce/yt/interface/common.h" namespace "NYT" nogil:
    cdef cppclass TAttributeFilter:
        TAttributeFilter() except +
        TAttributeFilter AddAttribute(TString)


cdef extern from "util/datetime/base.h" nogil:
    cdef cppclass TDuration:
        @staticmethod
        TDuration MilliSeconds(int)


cdef extern from "mapreduce/yt/interface/client_method_options.h" namespace "NYT" nogil:
    cdef enum ENodeType:
        NT_STRING
        NT_INT64
        NT_UINT64
        NT_DOUBLE
        NT_BOOLEAN
        NT_MAP
        NT_LIST
        NT_FILE
        NT_TABLE
        NT_DOCUMENT
        NT_REPLICATED_TABLE
        NT_TABLE_REPLICA

    cdef cppclass EAtomicity:
        pass

    cdef cppclass EDurability:
        pass

    cdef cppclass TInsertRowsOptions:
        TInsertRowsOptions() except +
        TInsertRowsOptions Atomicity(EAtomicity)
        TInsertRowsOptions Durability(EDurability)
        TInsertRowsOptions Update(bint)
        TInsertRowsOptions Aggregate(bint)
        TInsertRowsOptions RequireSyncReplica(bint)

    cdef cppclass TSelectRowsOptions:
        TSelectRowsOptions() except +
        TSelectRowsOptions Timeout(TDuration)
        TSelectRowsOptions InputRowLimit(i64)
        TSelectRowsOptions OutputRowLimit(i64)
        TSelectRowsOptions RangeExpansionLimit(ui64)
        TSelectRowsOptions FailOnIncompleteResult(bint)
        TSelectRowsOptions VerboseLogging(bint)
        TSelectRowsOptions EnableCodeCache(bint)

    cdef cppclass TLookupRowsOptions:
        TLookupRowsOptions() except +
        TLookupRowsOptions Timeout(TDuration)
        TLookupRowsOptions Columns(yvector[TString])
        TLookupRowsOptions KeepMissingRows(bint)

    cdef cppclass TCreateClientOptions:
        TCreateClientOptions() except +
        TCreateClientOptions Token(TString)

    cdef cppclass TCreateOptions:
        TCreateOptions() except +
        TCreateOptions Recursive(bint)
        TCreateOptions IgnoreExisting(bint)
        TCreateOptions Force(bint)
        TCreateOptions Attributes(TNode)

    cdef cppclass TGetOptions:
        TGetOptions() except +
        TGetOptions MaxSize(i64)
        TGetOptions AttributeFilter(TAttributeFilter)

    cdef cppclass TMountTableOptions:
        TMountTableOptions() except +
        TMountTableOptions FirstTabletIndex(i64)
        TMountTableOptions LastTabletIndex(i64)
        TMountTableOptions CellId(TTabletCellId)
        TMountTableOptions Freeze(bint)

    cdef cppclass TUnmountTableOptions:
        TUnmountTableOptions() except +
        TUnmountTableOptions FirstTabletIndex(i64)
        TUnmountTableOptions LastTabletIndex(i64)
        TUnmountTableOptions Force(bint)


cdef _to_cypress_node_type(s):
    if s == 'string_node':
        return NT_STRING
    elif s == 'int64_node':
        return NT_INT64
    elif s == 'uint64_node':
        return NT_UINT64
    elif s == 'double_node':
        return NT_DOUBLE
    elif s == 'boolean_node':
        return NT_BOOLEAN
    elif s == 'map_node':
        return NT_MAP
    elif s == 'list_node':
        return NT_LIST
    elif s == 'file':
        return NT_FILE
    elif s == 'table':
        return NT_TABLE
    elif s == 'document':
        return NT_DOCUMENT
    elif s == 'replicated_table':
        return NT_REPLICATED_TABLE
    elif s == 'table_replica':
        return NT_TABLE_REPLICA
    else:
        raise Exception('unknown cypress node type {}'.format(s))


cdef extern from "mapreduce/yt/interface/client_method_options.h" namespace "NYT::EAtomicity" nogil:
    cdef EAtomicity _None "NYT::EAtomicity::None"
    cdef EAtomicity Full


cdef extern from "mapreduce/yt/interface/client_method_options.h" namespace "NYT::EDurability" nogil:
    cdef EDurability Sync
    cdef EDurability Async


cdef extern from "mapreduce/yt/interface/fwd.h" namespace "NYT" nogil:
    cdef cppclass TNodeId:
        ui32 dw[4]

    cdef cppclass TTabletCellId:
        ui32 dw[4]

    cdef cppclass IClient:
        TNode Get(TString, TGetOptions) except +
        void Set(TString, TNode) except +
        bint Exists(TString) except +
        TNodeId Create(TString, ENodeType, TCreateOptions) except +
        void MountTable(TString, TMountTableOptions) except +
        void UnmountTable(TString, TUnmountTableOptions) except +
        void InsertRows(TString, yvector[TNode], TInsertRowsOptions) except +
        yvector[TNode] SelectRows(TString, TSelectRowsOptions) except +
        yvector[TNode] LookupRows(TString, yvector[TNode], TLookupRowsOptions) except +

    cdef cppclass IClientPtr:
        IClient operator*()


cdef extern from "mapreduce/yt/interface/client.h" namespace "NYT" nogil:
    cdef IClientPtr CreateClient(TString, TCreateClientOptions)


cdef class Client:
    cdef IClientPtr _client

    def __cinit__(self, proxy, token=None):
        cdef TCreateClientOptions opts
        if token:
            opts.Token(_to_TString(token))
        self._client = CreateClient(_to_TString(proxy), opts)

    def get(self, path, max_size=None, attributes=None):
        cdef TGetOptions opts
        cdef TAttributeFilter attrs_filter
        if max_size is not None:
            opts.MaxSize(<i64>max_size)
        if attributes is not None:
            for attr in attributes:
                attrs_filter.AddAttribute(_to_TString(attr))
            opts.AttributeFilter(attrs_filter)
        return _TNode_to_pyobj(cython.operator.dereference(self._client).Get(_to_TString(path), opts))

    def set(self, path, value):
        cython.operator.dereference(self._client).Set(_to_TString(path), _pyobj_to_TNode(value))

    def exists(self, path):
        return cython.operator.dereference(self._client).Exists(_to_TString(path))

    def create(self, type, path, recursive=False, ignore_existing=False, force=False, attributes=None):
        cdef TCreateOptions opts
        if recursive:
            opts.Recursive(True)
        if ignore_existing:
            opts.IgnoreExisting(True)
        if force:
            opts.Force(True)
        if attributes is not None:
            opts.Attributes(_pyobj_to_TNode(attributes))
        cdef TNodeId node_id = cython.operator.dereference(self._client).Create(
            _to_TString(path),
            _to_cypress_node_type(type),
            opts
        )
        return '-'.join(hex(node_id.dw[i])[2:] for i in xrange(4))

    def create_table(self, path, recursive=None, ignore_existing=False, attributes=None):
        return self.create('table', path, recursive=recursive, ignore_existing=ignore_existing, attributes=attributes)

    def mount_table(self, path, first_tablet_index=None, last_tablet_index=None, cell_id=None, freeze=None):
        cdef TMountTableOptions opts
        cdef TTabletCellId ci
        if first_tablet_index is not None:
            opts.FirstTabletIndex(<i64>first_tablet_index)
        if last_tablet_index is not None:
            opts.LastTabletIndex(<i64>last_tablet_index)
        if cell_id is not None:
            dw = cell_id.split('-')
            for i in xrange(4):
                ci.dw[i] = int(dw[i], 16)
            opts.CellId(ci)
        if freeze is not None:
            opts.Freeze(<bint>freeze)
        cython.operator.dereference(self._client).MountTable(_to_TString(path), opts)

    def unmount_table(self, path, first_tablet_index=None, last_tablet_index=None, force=False):
        cdef TUnmountTableOptions opts
        if first_tablet_index is not None:
            opts.FirstTabletIndex(<i64>first_tablet_index)
        if last_tablet_index is not None:
            opts.LastTabletIndex(<i64>last_tablet_index)
        if force:
            opts.Force(<bint>force)
        cython.operator.dereference(self._client).UnmountTable(_to_TString(path), opts)

    def insert_rows(self, path, rows, update=None, aggregate=None, atomicity=None, durability=None, require_sync_replica=None):
        cdef TInsertRowsOptions opts
        if update is not None:
            opts.Update(<bint>update)
        if aggregate is not None:
            opts.Aggregate(<bint>aggregate)
        if atomicity is not None:
            if atomicity == 'full':
                opts.Atomicity(Full)
            elif atomicity == 'none':
                opts.Atomicity(_None)
            else:
                raise Exception('wrong atomicity: {}'.format(atomicity))
        if durability is not None:
            if durability == 'sync':
                opts.Durability(Sync)
            elif durability == 'async':
                opts.Durability(Async)
            else:
                raise Exception('wrong durability: {}'.format(durability))
        if require_sync_replica is not None:
            opts.RequireSyncReplica(<bint>require_sync_replica)
        cdef TString cpath = _to_TString(path)
        cdef yvector[TNode] crows = _pyobj_to_TNode(rows).AsList()
        with nogil:
            cython.operator.dereference(self._client).InsertRows(cpath, crows, opts)

    def select_rows(self, query, input_row_limit=None, output_row_limit=None, range_expansion_limit=1000, fail_on_inclomplete_result=True, verbose_logging=False, enable_code_cache=True, timeout=None):
        cdef TSelectRowsOptions opts
        if input_row_limit is not None:
            opts.InputRowLimit(<i64>input_row_limit)
        if output_row_limit is not None:
            opts.OutputRowLimit(<i64>output_row_limit)
        opts.RangeExpansionLimit(<ui64>range_expansion_limit)
        opts.FailOnIncompleteResult(<bint>fail_on_inclomplete_result)
        opts.VerboseLogging(<bint>verbose_logging)
        opts.EnableCodeCache(<bint>enable_code_cache)
        if timeout is not None:
            opts.Timeout(TDuration.MilliSeconds(<int>timeout))
        cdef TString cquery = _to_TString(query)
        cdef yvector[TNode] rows
        with nogil:
            rows = cython.operator.dereference(self._client).SelectRows(cquery, opts)
        return [_TNode_to_pyobj(row) for row in rows]

    def lookup_rows(self, path, keys, timeout=None, columns=None, keep_missing_rows=False):
        cdef TLookupRowsOptions opts
        if timeout is not None:
            opts.Timeout(TDuration.MilliSeconds(<int>timeout))
        if columns is not None:
            opts.Columns(columns)
        opts.KeepMissingRows(<bint>keep_missing_rows)
        cdef TString cpath = _to_TString(path)
        cdef yvector[TNode] ckeys = _pyobj_to_TNode(keys).AsList()
        with nogil:
            rows = cython.operator.dereference(self._client).LookupRows(cpath, ckeys, opts)
        return [_TNode_to_pyobj(row) for row in rows]
