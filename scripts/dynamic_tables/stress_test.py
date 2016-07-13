#!/usr/bin/python

import yt.wrapper as yt
import yt.yson as yson
from yt.common import YtError
from yt.wrapper.table import TablePath
from yt.wrapper.client import Yt
from yt.wrapper.native_driver import make_request
import argparse
import random
from time import sleep
import sys

yt.config.VERSION = "v3"

class TInt64():
    def random(self):
        return yson.YsonInt64(random.randint(-2**63, 2**63 - 1))
    def str(self):
        return "int64"
    def comparable(self):
        return True
    def aggregatable(self):
        return ["sum", "min", "max"]
    def aggregate(self, function, lhs, rhs):
        if function == "sum":
            r = lhs + rhs
            if r < -2**63:
                r += 2**64
            elif r > 2**63 - 1:
                r -= 2**64
            return r
        elif function == "max":
            return max(lhs, rhs)
        elif function == "min":
            return min(lhs, rhs)

class TUnt64():
    def random(self):
        return yson.YsonUint64(random.randint(0, 2**64 - 1))
    def str(self):
        return "uint64"
    def comparable(self):
        return True
    def aggregatable(self):
        return ["sum", "min", "max"]
    def aggregate(self, function, lhs, rhs):
        if function == "sum":
            return (lhs + rhs) % 2**64
        elif function == "max":
            return max(lhs, rhs)
        elif function == "min":
            return min(lhs, rhs)

class TBoolean():
    def random(self):
        return yson.YsonBoolean(random.randint(0,1))
    def str(self):
        return "boolean"
    def comparable(self):
        return True
    def aggregatable(self):
        return None

class TDouble():
    def random(self):
        return yson.YsonDouble(random.uniform(-2**100,2**100))
    def str(self):
        return "double"
    def comparable(self):
        return True
    def aggregatable(self):
        return None

class TString():
    def random(self):
        def generate_string():
            length = random.randint(1,1000)
            return ''.join((chr(x) for x in (random.randint(0x41, 0x58) for i in xrange(length))))
            #return ''.join((chr(x) for x in (random.randint(0x21, 0x7d) for i in xrange(length))))
        return yson.YsonString(generate_string())
    def str(self):
        return "string"
    def comparable(self):
        return True
    def aggregatable(self):
        return ["min", "max"]
    def aggregate(self, function, lhs, rhs):
        if function == "max":
            return max(lhs, rhs)
        elif function == "min":
            return min(lhs, rhs)

class TAny():
    def random(self):
        return [{},{}]
    def str(self):
        return "any"
    def comparable(self):
        return False
    def aggregatable(self):
        return None

types = [TInt64(), TUnt64(), TBoolean(), TString(), TAny()]
key_types = [t for t in types if t.comparable()]

class Column():
    def __init__(self, ttype, name, sort_order=None, aggregate=None):
        self.type = ttype
        self.name = name
        self.sort_order = sort_order
        self.aggregate = aggregate
    def yson(self):
        y = {"name": self.name, "type": self.type.str()}
        if self.sort_order:
            y["sort_order"] = self.sort_order
        if self.aggregate:
            y["aggregate"] = self.aggregate
        return y
    def generate_value(self):
        return self.type.random()
    def do_aggregate(self, lhs, rhs):
        if lhs == None:
            return rhs
        elif rhs == None:
            return lhs
        else:
            return self.type.aggregate(self.aggregate, lhs, rhs)

class Schema():
    def __init__(self):
        self.appearance_probability = 0.9
        self.aggregate_probability = 0.5
        key_column_count = random.randint(1,10)
        data_column_count = random.randint(1,20)
        key_columns = [random.choice(key_types) for i in xrange(key_column_count)]
        data_columns = [random.choice(types) for i in xrange(data_column_count)]
        key_names = ["k%s" % str(i) for i in range(len(key_columns))]
        data_names = ["v%s" % str(i) for i in range(len(data_columns))]
        self.key_columns = [Column(t, n, "ascending") for (t,n) in zip(key_columns, key_names)]
        def aggr(t):
            def random_aggr(l):
                return l[random.randint(0, len(l) - 1)]
            return random_aggr(t.aggregatable()) if t.aggregatable() and random.random() < self.aggregate_probability else None
        self.data_columns = [Column(t, n, None, aggr(t)) for (t,n) in zip(data_columns, data_names)]
        self.columns = self.key_columns + self.data_columns
    def get_key_column_names(self):
        return [c.name for c in self.key_columns]
    def get_data_column_names(self):
        return [c.name for c in self.data_columns]
    def get_column_names(self):
        return [c.name for c in self.columns]
    def get_data_columns(self):
        return self.data_columns
    def yson(self):
        return [c.yson() for c in self.columns]
    def generate_pivot_key(self):
        return [c.generate_value() for c in self.key_columns]
    def generate_key(self):
        return self.generate_row_from_schema(self.key_columns)
    def generate_data(self):
        return self.generate_row_from_schema(self.data_columns)
    def generate_row(self):
        return self.generate_row_from_schema(self.columns)
    def generate_row_from_schema(self, columns):
        while True:
            result = {c.name: c.generate_value() for c in columns if random.random() < self.appearance_probability}
            if len(result) > 0:
                return result

class SchemafulMapper(object):
    def __init__(self, schema, table):
        #self.sleep_interval = args.sleep_interval
        #self.max_retry_count = args.max_retry_count
        self.sleep_interval = 120
        self.max_retry_count = 10
        self.schema = schema
        self.table = table

    def make_request(self, command, params, data, client):
        errors = []
        attempt = 0
        while attempt < self.max_retry_count:
            attempt += 1
            try:
                return make_request(command, params, data=data, client=client)
            except YtError as error:
                errors.append((attempt, str(error)))
                sleep(random.randint(1, self.sleep_interval))
        errors = ["try: %s\nerror:%s\n" % (attempt, err) for attempt, err in errors]
        errors = [e +  "\n\n===================================================================\n\n" for e in errors]
        stderr = "".join(errors)
        print >> sys.stderr, stderr
        #print >> sys.stderr, data
        raise Exception(" ".join(("Failed to execute command (%s attempts):" % attempt, command, str(params))))

    def prepare(self, value):
        if not isinstance(value, list):
            value = [value]
        return yson.dumps(value, yson_type="list_fragment", yson_format="text", boolean_as_string=False)

def create_keys(schema, dst, count, job_count):
    print "Generate random keys"
    class Mapper():
        def __init__(self, schema):
            self.schema = schema
        def __call__(self, record):
            for i in xrange(record["count"]):
                yield self.schema.generate_key()

    tmp = yt.create_temp_table()
    rows = [{"count": count/job_count} for i in xrange(job_count)]
    yt.write_table(tmp, rows, raw=False)
    yt.run_map(Mapper(schema), tmp, dst, spec={"job_count": job_count, "max_failed_job_count": 100})
    yt.run_sort(dst, sort_by=schema.get_key_column_names())
    def reducer(key, records):
        yield next(records)
    yt.run_reduce(reducer, dst, dst, reduce_by=schema.get_key_column_names())
    yt.remove(tmp)

def create_pivot_keys(schema, tablet_count):
    if tablet_count <= 1:
        return [[]]
    pivots = sorted([schema.generate_pivot_key() for i in xrange(tablet_count - 1)])
    unique_pivots = [pivots[0]]
    for pivot in pivots[1:]:
        if pivot != unique_pivots[-1]:
            unique_pivots.append(pivot)
    return [[]] + unique_pivots

def wait_until(path, state):
    while not all(x["state"] == state for x in yt.get(path + "/@tablets")):
        sleep(1)
def mount_table(path):
    yt.mount_table(path)
    wait_until(path, "mounted")
def unmount_table(path):
    yt.unmount_table(path)
    wait_until(path, "unmounted")

def create_dynamic_table(table, schema, attributes, tablet_count):
    print "Create dynamic table %s" % table
    attributes["dynamic"] = True
    attributes["schema"] = schema.yson()
    yt.create_table(table, attributes=attributes)
    owner = yt.get(table + "/@owner")
    yt.set(table + "/@acl", [{"permissions": ["mount"], "action": "allow", "subjects": [owner]}])
    yt.reshard_table(table, create_pivot_keys(schema, tablet_count))
    mount_table(table)

def reshard_table(table, schema, tablet_count):
    unmount_table(table)
    yt.reshard_table(table, create_pivot_keys(schema, tablet_count + random.randint(-3,3)))
    mount_table(table)

@yt.aggregator
class DataCreationMapper(SchemafulMapper):
    def __init__(self, schema, table, iteration):
        super(DataCreationMapper, self).__init__(schema, table)
        self.iteration = iteration
    def __call__(self, records):
        for record in records:
            for k in record.keys():
                if k[0] == '@':
                    record.pop(k)
            data = self.schema.generate_data()
            record.update(data)
            record["iteration"] = self.iteration
            yield record

def create_random_data(schema, key_table, iter_table, iteration, job_count):
    print "Generate random data, iteration %s" % iteration
    yt.run_map(
        DataCreationMapper(schema, iter_table, iteration),
        key_table,
        iter_table,
        spec={"job_count": job_count, "max_failed_job_count": 100},
        format=yt.YsonFormat(process_table_index=False, boolean_as_string=False))
    yt.run_sort(iter_table, sort_by=schema.get_key_column_names())

@yt.aggregator
class WriterMapper(SchemafulMapper):
    def __init__(self, schema, table, aggregate, update):
        super(WriterMapper, self).__init__(schema, table)
        self.aggregate = aggregate
        self.update = update
    def __call__(self, records):
        rows = []
        for record in records:
            for k in record.keys():
                if k[0] == '@' or k == "iteration":
                    record.pop(k)
            rows.append(record)

        config = {"driver_config_path": "/etc/ytdriver.conf", "api_version": "v3"}
        client = Yt(config=config)
        params = {
            "path": self.table,
            "input_format": "yson",
            "aggregate": self.aggregate,
            "update": self.update,
        }
        self.make_request("insert_rows", params, self.prepare(rows), client)

        if False:
            yield None

def write_data(schema, iter_table, table, iteration, aggregate, update, job_count):
    print "Write data, iteration %s" % iteration
    tmp_table = yt.create_temp_table()
    yt.run_map(
        WriterMapper(schema, table, aggregate, update),
        iter_table,
        tmp_table,
        spec={"job_count": job_count, "max_failed_job_count": 100},
        format=yt.YsonFormat(process_table_index=False, boolean_as_string=False))
    yt.remove(tmp_table)

class AggregateReducer:
    def __init__(self, schema, aggregate, update):
        self.schema = schema
        self.aggregate = aggregate
        self.update = update
        self.aggregates = {}
        if aggregate:
            for c in schema.get_data_columns():
                if c.aggregate:
                    self.aggregates[c.name] = c
    def __call__(self, key, records):
        records = list(records)
        records = sorted(records, key=lambda(x): x["@table_index"])
        record = dict(key)
        for c in self.schema.get_data_column_names():
            record[c] = None
        for r in records:
            for c in self.schema.get_data_column_names():
                if c not in r.keys() and self.update == False:
                    r[c] = None
                if c in r.keys():
                    if c in self.aggregates.keys():
                        record[c] = self.aggregates[c].do_aggregate(record[c], r[c])
                    elif c in self.schema.get_column_names():
                        record[c] = r[c]
        yield record

def aggregate_data(schema, data_table, iter_table, new_data_table, aggregate, update, job_count):
    print "Aggregate data"
    key = schema.get_key_column_names()
    yt.run_sort(data_table, sort_by=key + ["iteration"])
    yt.run_reduce(
        AggregateReducer(schema, aggregate, update),
        [data_table, iter_table],
        new_data_table,
        reduce_by=schema.get_key_column_names(),
        spec={"job_count": job_count, "max_failed_job_count": 100},
        format=yt.YsonFormat(boolean_as_string=False))

@yt.aggregator
class VerifierMapper(SchemafulMapper):
    def __init__(self, schema, table):
        super(VerifierMapper, self).__init__(schema, table)
    def __call__(self, records):
        config = {"driver_config_path": "/etc/ytdriver.conf", "api_version": "v3"}
        client = Yt(config=config)
        params = {
            "path": self.table,
            "input_format": "yson",
            "output_format": "yson",
            "keep_missing_rows": True,
        }

        records = list(records)
        keys = []
        for record in records:
            for k in record.keys():
                if k[0] == '@':
                    record.pop(k)
            key = {}
            for k in self.schema.get_key_column_names():
                key[k] = record[k]
            keys.append(key)

        data = self.make_request("lookup_rows", params, self.prepare(keys), client)
        results = yson.loads(data, yson_type="list_fragment")

        def equal(x, y):
            if (x == None) + (y == None) > 0:
                return (x == None) == (y == None)
            for c in self.schema.get_column_names():
                if ((c in x) != (c in y)) or ((c in x) and (x[c] != y[c])):
                    return False
            return True

        for i in xrange(len(keys)):
            record = records[i]
            result = next(results, None)
            if not equal(result, record):
                yield {"expected": record, "actual": result}

def verify(schema, data_table, table, result_table, job_count):
    print "Verify data"
    yt.run_map(
        VerifierMapper(schema, table),
        data_table,
        result_table,
        spec={"job_count": job_count, "max_failed_job_count": 100},
        format=yt.YsonFormat(process_table_index=False, boolean_as_string=False))
    rows = yt.read_table(result_table, raw=False)
    if next(rows, None) == None:
        print "Everything OK"
        return True
    else:
        print "FAILED, see %s" % result_table
        return False

def remove_existing(paths, force):
    for path in paths:
        if yt.exists(path):
            if force:
                yt.remove(path)
            else:
                raise Exception("Table %s already exists. Use --force" % path)

def single_iteration(schema, table, key_table, data_table, result_table, job_count, iterno, force, keep):
    aggregate_probability = 0.9
    update_probability = 0.5
    aggregate = random.random() < aggregate_probability
    update = random.random() < update_probability

    new_data_table = data_table + ".new"
    iter_table = table + ".iter.(%s-%s-%s)" % (iterno, aggregate, update)
    remove_existing([iter_table], force)

    create_random_data(schema, key_table, iter_table, iterno, job_count)
    write_data(schema, iter_table, table, iterno, aggregate, update, job_count)
    aggregate_data(schema, data_table, iter_table, new_data_table, aggregate, update, job_count)
    good = verify(schema, new_data_table, table, result_table, job_count)

    if good:
        yt.move(new_data_table, data_table, force=True)
    else:
        raise Exception("Verification failed at iteration %s" % iterno)
    return iter_table

def do_single_execution(table, schema, attributes, tablet_count, key_count, iterations, job_count, force, keep):
    key_table = table + ".keys"
    data_table = table + ".data"
    result_table = table + ".result"
    remove_existing([table, key_table, data_table, result_table], force)
    yt.create_table(data_table)

    create_dynamic_table(table, schema, attributes, tablet_count)
    create_keys(schema, key_table, key_count, job_count)

    iter_tables = []
    for i in xrange(iterations):
        iter_table = single_iteration(schema, table, key_table, data_table, result_table, job_count, i, force, keep)
        iter_tables.append(iter_table)
    unmount_table(table)
    mount_table(table)
    if not keep:
        for path in [table, key_table, data_table, result_table] + iter_tables:
            yt.remove(path)

def single_execution(table, schema, attributes, tablet_count, key_count, iterations, job_count, force, keep):
    try:
         do_single_execution(table, schema, attributes, tablet_count, key_count, iterations, job_count, force, keep)
    except Exception:
        pass

def variate_modes(table, args):
    schema = Schema()

    for optimize_for in ["lookup", "scan"]:
        single_execution(table + "." + optimize_for + ".none", schema, {"optimize_for": optimize_for}, args.tablet_count, args.key_count, args.iterations, args.job_count, args.force, args.keep)
        single_execution(table + "." + optimize_for + ".compressed", schema, {"optimize_for": optimize_for, "in_memory_mode": "compressed"}, args.tablet_count, args.key_count, args.iterations, args.job_count, args.force, args.keep)
        single_execution(table + "." + optimize_for + ".uncompressed", schema, {"optimize_for": optimize_for, "in_memory_mode": "uncompressed"}, args.tablet_count, args.key_count, args.iterations, args.job_count, args.force, args.keep)
        single_execution(table + "." + optimize_for + ".uncompressed.lookuptable", schema, {"optimize_for": optimize_for, "in_memory_mode": "uncompressed", "enable_lookup_hash_table": True}, args.tablet_count, args.key_count, args.iterations, args.job_count, args.force, args.keep)

def run_test(args):
    #for i in range(100):
        #variate_modes(args.table + "." + str(i), args)
    variate_modes(args.table, args)

def main():
    parser = argparse.ArgumentParser(description="Map-Reduce table manipulator.")
    parser.add_argument("--force", action="store_true", default=False, help="Overwrite destination table if it exists")
    parser.add_argument("--keep", action="store_true", default=False, help="Keep tables anyway")
    parser.add_argument("--table", type=str, help="Table path", required=True)
    parser.add_argument("--proxy", type=yt.config.set_proxy, help="YT proxy")
    parser.add_argument("--key_count", type=int, default=1000, help="Nuber of keys in dynamic table")
    parser.add_argument("--job_count", type=int, default=10, help="Nuber of jobs")
    parser.add_argument("--tablet_count", type=int, default=10, help="Nuber of tablets")
    parser.add_argument("--iterations", type=int, default=2, help="Nuber of iterations")
    args = parser.parse_args()

    run_test(args)

if __name__ == "__main__":
    main()
