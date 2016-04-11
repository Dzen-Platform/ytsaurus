var fs = require("fs");

var Q = require("bluebird");
var _ = require("underscore");

var YtApplicationOperations = require("../lib/application_operations").that;
var YtError = require("../lib/error").that;

////////////////////////////////////////////////////////////////////////////////

function clone(obj)
{
    return JSON.parse(JSON.stringify(obj));
}

function fixture(name)
{
    return JSON.parse(fs.readFileSync(__dirname + "/fixtures/" + name));
}

var CYPRESS_OPERATIONS = [
    fixture("cypress_19b5c14-c41a6620-7fa0d708-29a241d2.json"),
    fixture("cypress_1dee545-fe4c4006-cd95617-54f87a31.json"),
    fixture("cypress_d7df8-7d0c30ec-582ebd65-9ad7535a.json"),
];

var RUNTIME_OPERATIONS = {
    "1dee545-fe4c4006-cd95617-54f87a31": fixture("runtime_1dee545-fe4c4006-cd95617-54f87a31.json"),
};

var ARCHIVE_ITEMS = [
    fixture("archive_1.json"),
    fixture("archive_2.json"),
    fixture("archive_3.json"),
];

var ARCHIVE_COUNTS = [
    {user: "sandello", state: "completed", type: "map", count: 1},
    {user: "sandello", state: "failed", type: "map", count: 1},
    {user: "sandello", state: "aborted", type: "map", count: 1},
];

describe("YtApplicationOperations - list, get operations and scheduling info", function() {
    beforeEach(function(done) {
        this.logger = stubLogger();
        this.driver = { executeSimple: function() { return Q.resolve(); } };
        this.application_operations = new YtApplicationOperations(this.logger, this.driver);
        done();
    });

    function mockCypressForList(mock, result) {
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("list", sinon.match({path: "//sys/operations", attributes: sinon.match.any}))
            .returns(result);
    }

    function mockRuntimeForList(mock, result) {
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("get", sinon.match({path: "//sys/scheduler/orchid/scheduler/operations" }))
            .returns(result);
    }

    function mockArchiveItemsForList(mock, result) {
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("select_rows", sinon.match({
                query: sinon.match(/\* .* ORDER BY start_time/)
            }))
            .returns(result);
    }

    function mockArchiveCountersForList(mock, result) {
        if (!result) return;
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("select_rows", sinon.match({
                query: sinon.match(/user, state, type, sum\(1\) AS count .* GROUP BY .*/)
            }))
            .returns(result);
     }

    function mockCypressForGet(mock, id, result)
    {
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("get", sinon.match({path: "//sys/operations/" + id + "/@"}))
            .returns(result);
    }

    function mockRuntimeForGet(mock, id, result)
    {
        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("get", sinon.match({path: "//sys/scheduler/orchid/scheduler/operations/" + id}))
            .returns(result);
    }

    function mockArchiveForGet(mock, id, result)
    {
        var id_parts = YtApplicationOperations._idStringToUint64(id);
        var id_hi = id_parts[0];
        var id_lo = id_parts[1];

        mock
            .expects("executeSimple")
            .once()
            .withExactArgs("select_rows", sinon.match(function(params) {
                var query = params.query;
                var regexp = new RegExp(
                    "WHERE \\(id_hi, id_lo\\) = " +
                    "\\(" + id_hi.toString(10) + "u, " + id_lo.toString(10) + "u\\)");
                return query && regexp.test(query);
            }))
            .returns(result);
    }

    function mockForList(mock, cypress_result, runtime_result, archive_items_result, archive_counters_result)
    {
        mockCypressForList(mock, cypress_result);
        mockRuntimeForList(mock, runtime_result);
        mockArchiveItemsForList(mock, archive_items_result);
        mockArchiveCountersForList(mock, archive_counters_result);
    }

    function mockForGet(mock, id, cypress_result, runtime_result, archive_result)
    {
        mockCypressForGet(mock, id, cypress_result);
        mockRuntimeForGet(mock, id, runtime_result);
        mockArchiveForGet(mock, id, archive_result);
    }

    it("should fail when cypress is not available", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.reject(), Q.resolve({}), Q.resolve([]), Q.resolve([]));
        this.application_operations.list({}).then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/failed to list operations/i);
            })
        .then(done, done);
    });

    it("should fail when max_size is invalid", function(done) {
        this.application_operations.list({
            max_size: 999999
        }).then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/maximum result size exceedes allowed limit/i);
            })
        .then(done, done);
    });

    it("should fail when from_time & to_time are invalid", function(done) {
        this.application_operations.list({
            from_time: "2015-01-01T00:00:00Z",
            to_time: "2015-12-12T00:00:00Z"
        }).then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/time span exceedes allowed limit/i);
            })
        .then(done, done);
    });

    it("should fail when cursor_time is out of range (before from_time)", function(done) {
        this.application_operations.list({
            from_time: "2015-01-01T01:00:00Z",
            to_time: "2015-01-01T02:00:00Z",
            cursor_time: "2015-01-01T00:00:00Z",
        }).then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/time cursor is out of range/i);
            })
        .then(done, done);
    });

    it("should fail when cursor_time is out of range (after to_time)", function(done) {
        this.application_operations.list({
            from_time: "2015-01-01T01:00:00Z",
            to_time: "2015-01-01T02:00:00Z",
            cursor_time: "2015-01-01T03:00:00Z",
        }).then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/time cursor is out of range/i);
            })
        .then(done, done);
    });

    it("should list operations from cypress without filters", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
                "1dee545-fe4c4006-cd95617-54f87a31",
                "19b5c14-c41a6620-7fa0d708-29a241d2",
            ]);
            expect(result.operations[1].$attributes.brief_progress.jobs.completed).to.eql(9618);
            expect(result.operations[1].$attributes.start_time).to.eql("2016-03-02T05:43:43.104532Z");
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with from_time & to_time filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-03-02T00:00:00Z",
            to_time: "2016-03-02T12:00:00Z",
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1});
            expect(result.state_counts).to.deep.equal({running: 1});
            expect(result.type_counts).to.deep.equal({map: 1});
            expect(result.failed_jobs_count).to.deep.equal(0);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "1dee545-fe4c4006-cd95617-54f87a31",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with cursor_time/past filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            cursor_time: "2016-03-02T12:00:00Z",
            cursor_direction: "past",
        }).then(function(result) {
            // counters are intact
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            // result list is reduced
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "1dee545-fe4c4006-cd95617-54f87a31",
                "19b5c14-c41a6620-7fa0d708-29a241d2",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with cursor_time/filter filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            cursor_time: "2016-03-02T00:00:00Z",
            cursor_direction: "future",
        }).then(function(result) {
            // counters are intact
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            // result list is reduced
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "1dee545-fe4c4006-cd95617-54f87a31",
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with type filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            type: "map_reduce"
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with state filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            state: "completed"
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 1});
            expect(result.failed_jobs_count).to.deep.equal(0);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "19b5c14-c41a6620-7fa0d708-29a241d2",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with user filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            user: "psushin"
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({running: 1});
            expect(result.type_counts).to.deep.equal({map: 1});
            expect(result.failed_jobs_count).to.deep.equal(0);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "1dee545-fe4c4006-cd95617-54f87a31",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with text filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            filter: "MRPROC"
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({failed: 1});
            expect(result.type_counts).to.deep.equal({map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress with failed jobs filter", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            with_failed_jobs: true
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
            ]);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations from cypress without counters", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            include_counters: false
        }).then(function(result) {
            expect(Object.keys(result)).to.deep.eql(["operations"]);
            expect(result.operations.length).to.eq(3);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations w.r.t. max_size parameter (incomplete result)", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            max_size: 1
        }).then(function(result) {
            expect(result.operations.$incomplete).to.eq(true);
            expect(result.operations.$value.length).to.eq(1);
            mock.verify();
        })
        .then(done, done);
    });

    it("should list operations w.r.t. max_size parameter (complete result)", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
            max_size: 3
        }).then(function(result) {
            expect(result.operations.length).to.eq(3);
            mock.verify();
        })
        .then(done, done);
    });

    it("should override progress with runtime information", function(done) {
        var mock = sinon.mock(this.driver);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.resolve(RUNTIME_OPERATIONS), Q.reject(), Q.reject());
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1});
            expect(result.state_counts).to.deep.equal({completed: 1, failed: 1, running: 1});
            expect(result.type_counts).to.deep.equal({map: 2, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
                "1dee545-fe4c4006-cd95617-54f87a31",
                "19b5c14-c41a6620-7fa0d708-29a241d2",
            ]);
            expect(result.operations[1].$attributes.brief_progress.jobs.completed).to.eql(9725);
            expect(result.operations[1].$attributes.start_time).to.eql("2016-03-02T05:43:43.104532Z");
            mock.verify();
        })
        .then(done, done);
    });

    it("should merge with archive information", function(done) {
        var mock = sinon.mock(this.driver);
        var archive_items = clone(ARCHIVE_ITEMS);
        var archive_counts = clone(ARCHIVE_COUNTS);
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.resolve(archive_items), Q.resolve(archive_counts));
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1, sandello: 3});
            expect(result.state_counts).to.deep.equal({aborted: 1, completed: 2, failed: 2, running: 1});
            expect(result.type_counts).to.deep.equal({map: 5, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
                "1dee545-fe4c4006-cd95617-54f87a31",
                "19b5c14-c41a6620-7fa0d708-29a241d2",
                "303b02-bc6c8994-778328e8-511a3048", // archived
                "12ad62b-6bc1ed2f-ed7b018e-8633b5cd", // archived
                "12a7385-20d240f6-78421110-8a351d84", // archived
            ]);
            expect(result.operations[1].$attributes.brief_progress.jobs.completed).to.eql(9725);
            expect(result.operations[1].$attributes.start_time).to.eql("2016-03-02T05:43:43.104532Z");
            expect(result.operations[3].$attributes.start_time).to.eql("2016-01-28T13:31:03.127Z");
            expect(result.operations[3].$attributes.finish_time).to.eql("2016-01-28T13:36:19.059Z");
            expect(result.operations[3].$attributes.filter_factors).to.eql(undefined);
            mock.verify();
        })
        .then(done, done);
    });

    it("should not override data from cypress with archive information", function(done) {
        var mock = sinon.mock(this.driver);
        var archive_items = clone(ARCHIVE_ITEMS);
        var archive_counts = clone(ARCHIVE_COUNTS);
        // would override
        // 19b5c14-c41a6620-7fa0d708-29a241d2
        // ignat, completed, map
        // would remove
        // 12ad62b-6bc1ed2f-ed7b018e-8633b5cd
        // sandello, completed, map
        archive_items[0].id_hash = null;
        archive_items[0].id_hi.$value = "3000032674122356488";
        archive_items[0].id_lo.$value = "14130719068480298004";
        mockForList(mock, Q.resolve(CYPRESS_OPERATIONS), Q.reject(), Q.resolve(archive_items), Q.resolve(archive_counts));
        this.application_operations.list({
            from_time: "2016-02-25T00:00:00Z",
            to_time: "2016-03-04T00:00:00Z",
        }).then(function(result) {
            expect(result.user_counts).to.deep.equal({psushin: 1, ignat: 1, data_quality_robot: 1, sandello: 2});
            expect(result.state_counts).to.deep.equal({aborted: 1, completed: 1, failed: 2, running: 1});
            expect(result.type_counts).to.deep.equal({map: 4, map_reduce: 1});
            expect(result.failed_jobs_count).to.deep.equal(1);
            expect(result.operations.map(function(item) { return item.$value; })).to.deep.equal([
                "d7df8-7d0c30ec-582ebd65-9ad7535a",
                "1dee545-fe4c4006-cd95617-54f87a31",
                "19b5c14-c41a6620-7fa0d708-29a241d2",
                "303b02-bc6c8994-778328e8-511a3048",
                "12a7385-20d240f6-78421110-8a351d84",
            ]);
            expect(result.operations[2].$attributes.brief_progress.jobs.completed).to.eql(1);
            expect(result.operations[2].$attributes.authenticated_user).to.eql("ignat");
            expect(result.operations[2].$attributes.filter_factors).to.eql(undefined);
            mock.verify();
        })
        .then(done, done);
    });

    it("should report an error on missing required parameters for _get", function(done) {
        this.application_operations.get({})
        .then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/required parameter/);
            }
        )
        .then(done, done);
    });

    it("should report an error for malformed ids for _get", function(done) {
        this.application_operations.get({id: "bazzinga"})
        .then(
            function() { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                err.message.should.match(/unable to parse operation id/i);
            }
        )
        .then(done, done);
    });

    it("should propagate error on _get", function(done) {
        var mock = sinon.mock(this.driver);

        var id = "2ec4d6a3-f53d20bd-9083e069-2e728b62";
        var cypress_result = Q.reject(new YtError("Cypress has failed"));
        var runtime_result = Q.reject(new YtError("Runtime has failed"));
        var archive_result = Q.reject(new YtError("Archive has failed"));

        mockForGet(mock, id, cypress_result, runtime_result, archive_result);

        this.application_operations.get({id: id})
        .then(
            function(result) { throw new Error("This should fail."); },
            function(err) {
                err.should.be.instanceof(YtError);
                mock.verify();
            })
        .then(done, done);
    });

    it("should get operation from cypress", function(done) {
        var mock = sinon.mock(this.driver);

        var id = "2ec4d6a3-f53d20bd-9083e069-2e728b62";

        var cypress_result = Q.resolve({"other_field": "abc"});
        var runtime_result = Q.reject();
        var archive_result = Q.resolve([]);

        mockForGet(mock, id, cypress_result, runtime_result, archive_result);

        this.application_operations.get({id: id})
        .then(function(result) {
            result.should.deep.equal({"other_field": "abc"});
            mock.verify();
        })
        .then(done, done);
    });

    it("should get operation from cypress & runtime", function(done) {
        var mock = sinon.mock(this.driver);

        var id = "2ec4d6a3-f53d20bd-9083e069-2e728b62";

        var cypress_result = Q.resolve({"other_field": "abc"});
        var runtime_result = Q.resolve({"blah_field": "cde", "other_field": "xxx"});
        var archive_result = Q.resolve([]);

        mockForGet(mock, id, cypress_result, runtime_result, archive_result);

        this.application_operations.get({id: id})
        .then(function(result) {
            result.should.deep.equal({"other_field": "xxx", "blah_field": "cde"});
            mock.verify();
        })
        .then(done, done);
    });

    it("should get operation from archive", function(done) {
        var mock = sinon.mock(this.driver);

        var id = "2ec4d6a3-f53d20bd-9083e069-2e728b62";
        var id_parts = YtApplicationOperations._idStringToUint64(id);
        var id_hi = id_parts[0];
        var id_lo = id_parts[1];

        var cypress_result = Q.reject(
            new YtError("Can't find operation in Cypress").withCode(500));
        var runtime_result = Q.reject(
            new YtError("Scheduler does not want to cooperate"));
        var archive_result = Q.resolve([{
            "id_hi": id_hi.toString(10),
            "id_lo": id_lo.toString(10),
            "id_hash": "hash",
            "other_field": "abc"
        }]);

        mockForGet(mock, id, cypress_result, runtime_result, archive_result);

        this.application_operations.get({id: id})
        .then(function(result) {
            result.should.deep.equal({"other_field": "abc"});
            mock.verify();
        })
        .then(done, done);
    });

});

