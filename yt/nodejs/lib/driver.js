var util = require("util");

var buffertools = require("buffertools");
var Q = require("bluebird");
var _ = require("underscore");

var YtError = require("./error").that;
var YtReadableStream = require("./readable_stream").that;
var YtWritableStream = require("./writable_stream").that;

var binding = require("./ytnode");
var utils = require("./utils");

////////////////////////////////////////////////////////////////////////////////

var __DBG = require("./debug").that("B", "Driver");

var _SIMPLE_EXECUTE_USER = "root";
var _SIMPLE_EXECUTE_FORMAT = binding.CreateV8Node("json");

////////////////////////////////////////////////////////////////////////////////

function promisinglyPipe(source, destination)
{
    return new Q(function(resolve, reject) {
        var debug = __DBG.Tagged("Pipe");
        var clean = false;

        function resolve_and_clear() {
            if (!clean) {
                cleanup();
                clean = true;
            }
            resolve();
        }

        function reject_and_clear(err) {
            if (!clean) {
                cleanup();
                clean = true;
            }
            reject(err);
        }

        function on_data(chunk) {
            if (destination.writable && destination.write(chunk) === false) {
                source.pause();
            }
        }

        source.on("data", on_data);

        function on_drain() {
            debug("on_drain");
            if (source.readable) {
                source.resume();
            }
        }

        destination.on("drain", on_drain);

        function on_end() {
            debug("Piping has ended");
            resolve_and_clear();
        }
        function on_source_close() {
            debug("Source stream has been closed");
            reject_and_clear(new YtError("Source stream in the pipe has been closed"));
        }
        function on_destination_close() {
            debug("Destination stream has been closed");
            reject_and_clear(new YtError("Destination stream in the pipe has been closed"));
        }
        function on_error(err) {
            debug("An error occured");
            reject_and_clear(err);
        }

        source.on("end", on_end);
        source.on("close", on_source_close);
        source.on("error", on_error);

        destination.on("close", on_destination_close);
        destination.on("error", on_error);

        function cleanup() {
            debug("Cleaning up");

            source.removeListener("data", on_data);
            destination.removeListener("drain", on_drain);

            source.removeListener("end", on_end);
            source.removeListener("close", on_source_close);
            source.removeListener("error", on_error);

            destination.removeListener("close", on_destination_close);
            destination.removeListener("error", on_error);

            source.removeListener("end", cleanup);
            source.removeListener("close", cleanup);

            destination.removeListener("end", cleanup);
            destination.removeListener("close", cleanup);
        }

        destination.emit("pipe", source);
    });
}

function parseJsonRows(rowData) {
    // Last row is in fact empty string
    var rows = rowData
        .split('\n')
        .slice(0, -1);

    rows = _.map(rows, JSON.parse);
    return rows;
}

////////////////////////////////////////////////////////////////////////////////

function YtDriver(config, echo)
{
    this.__DBG = __DBG.Tagged();

    this.low_watermark = config.low_watermark;
    this.high_watermark = config.high_watermark;

    this.__DBG("low_watermark = " + this.low_watermark);
    this.__DBG("high_watermark = " + this.high_watermark);

    this._binding = new binding.TDriverWrap(!!echo, config.proxy);

    this.__DBG("New");
}

YtDriver.prototype.execute = function(
    name, user,
    input_stream, input_compression,
    output_stream, output_compression,
    parameters, request_id, pause,
    response_parameters_consumer,
    result_interceptor)
{
    this.__DBG("execute");

    var wrapped_input_stream = new YtWritableStream(this.low_watermark, this.high_watermark);
    var wrapped_output_stream = new YtReadableStream(this.high_watermark);

    var deferred = Q.defer();
    var self = this;

    var destroyer_state = {
        fired: false,
        objects: [wrapped_input_stream, wrapped_output_stream, input_stream, output_stream]
    };

    function destroyer() {
        if (!destroyer_state.fired) {
            for (var i = 0, n = destroyer_state.objects.length; i < n; ++i) {
                destroyer_state.objects[i].destroy();
            }
            destroyer_state.objects = null;
            destroyer_state.fired = true;
        }
    }

    function undestroyer() {
        destroyer_state.objects = null;
        destroyer_state.fired = true;
    }

    var input_pipe_promise = promisinglyPipe(input_stream, wrapped_input_stream)
        .then(
        function ip_promise_then() {
            self.__DBG("execute -> input_pipe_promise has been resolved");
            wrapped_input_stream.end();
        },
        function ip_promise_catch(err) {
            self.__DBG("execute -> input_pipe_promise has been rejected");
            destroyer();
            deferred.reject(new YtError("Input pipe has been canceled", err));
        });

    var output_pipe_promise = promisinglyPipe(wrapped_output_stream, output_stream)
        .then(
        function op_promise_then() {
            // Do not close |output_stream| here since we have to write out trailers.
            self.__DBG("execute -> output_pipe_promise has been resolved");
        },
        function op_promise_catch(err) {
            self.__DBG("execute -> output_pipe_promise has been rejected");
            destroyer();
            deferred.reject(new YtError("Output pipe has been canceled", err));
        });

    var canceler = this._binding.Execute(name, user,
        wrapped_input_stream._binding, input_compression,
        wrapped_output_stream._binding, output_compression,
        parameters, request_id,
        function(result) {
            self.__DBG("execute -> (on-execute callback)");

            if (typeof(result_interceptor) === "function") {
                self.__DBG("execute -> (interceptor)");
                try {
                    result_interceptor(result);
                } catch (ex) {
                }
            }

            if (result.code === 0) {
                self.__DBG("execute -> execute_promise has been resolved");
                deferred.resolve(Array.prototype.slice.call(arguments));
            } else {
                self.__DBG("execute -> execute_promise has been rejected");
                deferred.reject(result);
            }
        },
        response_parameters_consumer);

    input_pipe_promise.error(function() { canceler.Cancel(); });

    output_pipe_promise.error(function() { canceler.Cancel(); });

    process.nextTick(function() { pause.unpause(); });

    return Q
        .all([deferred.promise, input_pipe_promise, output_pipe_promise])
        .spread(function(result, ir, or) {
            undestroyer();
            return result;
        });
};

YtDriver.prototype.executeSimple = function(name, parameters, data)
{
    this.__DBG("executeSimple");

    var input_stream = new utils.MemoryInputStream(data && JSON.stringify(data));
    var output_stream = new utils.MemoryOutputStream();
    var pause = utils.Pause(input_stream);

    parameters.input_format = "json";
    parameters.output_format = parameters.output_format || "json";

    var descriptor = this.find_command_descriptor(name);

    return this.execute(name, _SIMPLE_EXECUTE_USER,
        input_stream, binding.ECompression_None,
        output_stream, binding.ECompression_None,
        binding.CreateV8Node(parameters), null, pause, function(){})
    .then(function(result) {
        var body = buffertools.concat.apply(undefined, output_stream.chunks);
        if (descriptor.output_type === "tabular") {
            return parseJsonRows(body.toString());
        } else if (body.length) {
            return JSON.parse(body);
        }
    });
};

YtDriver.prototype.find_command_descriptor = function(name)
{
    this.__DBG("find_command_descriptor");
    return this._binding.FindCommandDescriptor(name);
};

YtDriver.prototype.get_command_descriptors = function()
{
    this.__DBG("get_command_descriptors");
    return this._binding.GetCommandDescriptors();
};

////////////////////////////////////////////////////////////////////////////////

exports.that = YtDriver;
