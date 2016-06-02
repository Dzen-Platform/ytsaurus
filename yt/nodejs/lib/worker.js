// Dependencies.
var cluster = require("cluster");
var fs = require("fs");
var http = require("http");
var https = require("https");
var os = require("os");

var yt = require("yt");

var connect = require("connect");
var node_static = require("node-static");
var Q = require("bluebird");

var v8_profiler = require("profiler");
var v8_heapdump = require("heapdump");

var binding = require("./ytnode");

// Debugging stuff.
var __DBG = require("./debug").that("C", "Cluster Worker");
var __PROFILE = false;

// Load configuration.
var config = JSON.parse(process.env.YT_PROXY_CONFIGURATION);

// Cache hostname.
var os_hostname = os.hostname();
var hostname = config.fqdn || os_hostname;

// Set up logging (the hard way).
var logger_mediate = function(level, message, payload) {
    // Capture real message time before sending an event.
    payload = payload || {};

    var record = {};
    record.hostname = hostname;
    record.pid = process.pid;
    record.wid = cluster.worker.id;
    record.time = new Date().toISOString();
    record.message = message;
    Object.keys(payload).forEach(function(key) {
        if (!record.hasOwnProperty(key)) {
            record[key] = payload[key];
        }
    });

    process.send({
        type: "log",
        level: level,
        json: JSON.stringify(record),
    });
};

var profiler_mediate = function(method, metric, tags, value) {
    process.send({
        type: "profile",
        method: method,
        metric: metric,
        tags: tags,
        value: value,
    });
};

var logger = {
    debug: function(message, payload) {
        return logger_mediate("debug", message, payload);
    },
    info: function(message, payload) {
        return logger_mediate("info", message, payload);
    },
    warn: function(message, payload) {
        return logger_mediate("warn", message, payload);
    },
    error: function(message, payload) {
        return logger_mediate("error", message, payload);
    },
};

var unbuffered_profiler = {
    inc: function(metric, tags, value) {
        return profiler_mediate("inc", metric, tags, value);
    },
    upd: function(metric, tags, value) {
        return profiler_mediate("upd", metric, tags, value);
    },
};

var buffered_profiler = new yt.YtStatistics();

setInterval(function() {
    buffered_profiler.mergeTo(unbuffered_profiler);
}, 1000);

// Setup periodic V8 info dump.
var last_gc_scavenge_count = 0;
var last_gc_scavenge_time = 0;
var last_gc_mark_sweep_compact_count = 0;
var last_gc_mark_sweep_compact_time = 0;

setInterval(function() {
    var statistics = binding.GetGCStatistics();

    unbuffered_profiler.inc(
        "yt.http_proxy.gc_scavenge_count", {},
        statistics.total_scavenge_count - last_gc_scavenge_count);
    last_gc_scavenge_count = statistics.total_scavenge_count;

    unbuffered_profiler.inc(
        "yt.http_proxy.gc_scavenge_time", {},
        statistics.total_scavenge_time - last_gc_scavenge_time);
    last_gc_scavenge_time = statistics.total_scavenge_time;

    unbuffered_profiler.inc(
        "yt.http_proxy.mark_sweep_compact_count", {},
        statistics.total_mark_sweep_compact_count - last_gc_mark_sweep_compact_count);
    last_gc_mark_sweep_compact_count = statistics.total_mark_sweep_compact_count;

    unbuffered_profiler.inc(
        "yt.http_proxy.mark_sweep_compact_time", {},
        statistics.total_mark_sweep_compact_time - last_gc_mark_sweep_compact_time);
    last_gc_mark_sweep_compact_time = statistics.total_mark_sweep_compact_time;
}, 1000);

setInterval(function() {
    var statistics = binding.GetHeapStatistics();

    unbuffered_profiler.upd("yt.http_proxy.heap.total", {}, statistics.total_heap_size);
    unbuffered_profiler.upd("yt.http_proxy.heap.total_exec", {}, statistics.total_heap_size_executable);
    unbuffered_profiler.upd("yt.http_proxy.heap.used", {}, statistics.used_heap_size);
    unbuffered_profiler.upd("yt.http_proxy.heap.limit", {}, statistics.heap_size_limit);
}, 1000);

var version;

try {
    version = JSON.parse(fs.readFileSync(__dirname + "/../package.json"));
} catch (ex) {
    version = { version : "(development)", versionFull: "(development)", dependencies : {} };
}

// TODO(sandello): Extract these settings somewhere.
if (typeof(config.low_watermark) === "undefined") {
    config.low_watermark = parseInt(0.80 * config.memory_limit, 10);
}

if (typeof(config.high_watermark) === "undefined") {
    config.high_watermark = parseInt(0.95 * config.memory_limit, 10);
}

// TODO(sandello): Extract singleton configuration to a separate branch.
yt.configureSingletons(config.proxy);

yt.YtRegistry.set("fqdn", config.fqdn || require("os").hostname());
yt.YtRegistry.set("port", config.port);
yt.YtRegistry.set("config", config);
yt.YtRegistry.set("logger", logger);
yt.YtRegistry.set("profiler", buffered_profiler);
yt.YtRegistry.set("driver", new yt.YtDriver(config));
yt.YtRegistry.set("authority", new yt.YtAuthority(
    config.authentication,
    yt.YtRegistry.get("driver")));
yt.YtRegistry.set("coordinator", new yt.YtCoordinator(
    config.coordination,
    logger,
    yt.YtRegistry.get("driver"),
    yt.YtRegistry.get("fqdn"),
    yt.YtRegistry.get("port")));
yt.YtRegistry.set("eio_watcher", new yt.YtEioWatcher(logger, buffered_profiler, config));

// Hoist variable declaration.
var application;

var insecure_server;
var secure_server;

var insecure_listening_deferred = Q.defer();
var secure_listening_deferred = Q.defer();

var insecure_close_deferred = Q.defer();
var secure_close_deferred = Q.defer();

var violentlyDieTriggered = false;
var violentlyDie = function violentDeath() {
    if (violentlyDieTriggered) { return; }
    violentlyDieTriggered = true;

    process.nextTick(function() {
        cluster.worker.disconnect();
        cluster.worker.destroy();
        process.exit();
    });
};

var gracefullyDieTriggered = false;
var gracefullyDie = function gracefulDeath() {
    if (gracefullyDieTriggered) { return; }
    gracefullyDieTriggered = true;

    logger.info("Prepairing to die", { wid : cluster.worker.id, pid : process.pid });

    buffered_profiler.mergeTo(unbuffered_profiler);
    process.send({ type : "stopping" });

    try {
        if (!!insecure_server) {
            insecure_server.close();
        } else {
            secure_close_deferred.resolve();
        }
    } catch (ex) {
        logger.error("Caught exception during HTTP shutdown: " + ex.toString());
    }
    try {
        if (!!secure_server) {
            secure_server.close();
        } else {
            secure_close_deferred.resolve();
        }
    } catch (ex) {
        logger.error("Caught exception during HTTP shutdown: " + ex.toString());
    }
};

// Fire up the heart.
var supervisor_liveness;

if (!__DBG.On) {
    (function sendHeartbeat() {
        process.send({ type : "heartbeat" });
        setTimeout(sendHeartbeat, 2000);
    }());

    supervisor_liveness = setTimeout(gracefullyDie, 30000);
}

// Setup watchdog.
setInterval(function() {
    var statistics = binding.GetHeapStatistics();
    if (statistics.used_heap_size > 128 * 1024 * 1024) {
        console.error("[" + process.pid + "] Heap is >128MB; gracefully restarting...");
        gracefullyDie();
    }
}, 5000);

// Setup signal handlers.
process.on("SIGUSR1", function() {
    console.error("[" + process.pid + "] Writing V8 heap dump...");
    try {
        v8_heapdump.writeSnapshot();
    } catch (ex) {
        console.error("Caught exception: " + ex.toString());
    }

    console.error("[" + process.pid + "] Dumping Jemalloc profile...");
    try {
        binding.JemallocCtlWrite("prof.dump", null);
    } catch (ex) {
        console.error("Caught exception: " + ex.toString());
    }

    console.error("[" + process.pid + "] Dumping Jemalloc statistics...");
    try {
        binding.JemallocPrintStats();
    } catch (ex) {
        console.error("Caught exception: " + ex.toString());
    }
});

// Setup message handlers.
process.on("message", function(message) {
    if (!message || !message.type) {
        return; // Improper message format.
    }

    switch (message.type) {
        case "heartbeat":
            if (supervisor_liveness) {
                clearTimeout(supervisor_liveness);
            }
            supervisor_liveness = setTimeout(gracefullyDie, 15000);
            break;
        case "gracefullyDie":
            gracefullyDie();
            break;
        case "violentlyDie":
            violentlyDie();
            break;
    }
});

process.on("exit", function() {
    // Call _exit to avoid subtle shutdown issues.
    binding.WipeOutCurrentProcess(0);
});

process.on("uncaughtException", function(err) {
    console.error("*** Uncaught Exception ***");
    console.error(err);
    if (err.trace) {
        console.error(err.trace);
    }
    if (err.stack) {
        console.error(err.stack);
    }
    // Call _exit to avoid subtle shutdown issues.
    binding.WipeOutCurrentProcess(1);
});

// Fire up the head.
logger.info("Starting HTTP proxy worker", { wid : cluster.worker.id, pid : process.pid });

application = connect()
    .use(yt.YtLogRequest())
    .use(yt.YtAcao())
    .use(yt.YtCheckPythonWrapperVersion())
    .use(connect.favicon())
    .use("/hosts", yt.YtApplicationHosts())
    .use("/auth", yt.YtApplicationAuth())
    .use("/upravlyator", yt.YtApplicationUpravlyator())
    // TODO(sandello): This would be deprecated with nodejs 0.10.
    // Begin of asynchronous middleware.
    .use(function(req, rsp, next) {
        req.pauser = yt.Pause(req);
        next();
    })
    .use(yt.YtAuthentication())
    .use("/api", yt.YtApplicationApi())
    .use(function(req, rsp, next) {
        process.nextTick(function() { req.pauser.unpause(); });
        next();
    })
    // End of asynchronous middleware.
    .use("/gc", function(req, rsp, next) {
        if (req.url === "/") {
            if (global.gc) {
                global.gc();
                rsp.statusCode = 200;
            } else {
                rsp.statusCode = 500;
            }
            return void yt.utils.dispatchAs(rsp, "");
        }
        next();
    })
    .use("/ping", function(req, rsp, next) {
        if (req.url === "/") {
            var isSelfAlive = yt.YtRegistry.get("coordinator").isSelfAlive();
            rsp.statusCode = isSelfAlive ? 200 : 503;
            return void yt.utils.dispatchAs(rsp, "");
        }
        next();
    })
    .use("/version", function(req, rsp, next) {
        if (req.url === "/") {
            return void yt.utils.dispatchAs(rsp, version.versionFull, "text/plain");
        }
        next();
    });

config.redirect.forEach(function(site) {
    var web_path = site[0].replace(/\/+$/, "");
    var real_path = site[1].replace(/\/+$/, "");
    application.use(web_path, function(req, rsp, next) {
        return void yt.utils.redirectTo(rsp, real_path + req.url, 301);
    });
});

config.static.forEach(function(site) {
    var web_path = site[0].replace(/\/+$/, "");
    var real_path = site[1];
    var server = new node_static.Server(real_path, {
        cache: 3600,
        gzip: true,
    });
    application.use(web_path, function(req, rsp, next) {
        if (req.url === "/") {
            if (req.originalUrl === web_path) {
                return void yt.utils.redirectTo(rsp, web_path + "/");
            }
            req.url = "index.html";
        }
        req.on("end", function() {
            server.serve(req, rsp);
        });
    });
});

application
    .use("/", function(req, rsp, next) {
        if (req.url === "/") {
            return void yt.utils.redirectTo(rsp, "/ui/");
        }
        next();
    })
    .use(function(req, rsp) {
        rsp.statusCode = 404;
        return void yt.utils.dispatchAs(
            rsp, "Invalid URI " + JSON.stringify(req.url) + ". " +
            "Please refer to documentation at http://wiki.yandex-team.ru/YT/ " +
            "to learn more about HTTP API.",
            "text/plain");
    });

// Bind servers.
if (config.port && config.address) {
    logger.info("Binding to insecure socket", {
        wid: cluster.worker.id,
        pid: process.pid,
        address: config.address,
        port: config.port
    });

    insecure_server = http.createServer(application);
    insecure_server.listen(config.port, config.address);

    insecure_server.on("close", insecure_close_deferred.resolve.bind(insecure_close_deferred));
    insecure_server.on("listening", insecure_listening_deferred.resolve.bind(insecure_listening_deferred));
    insecure_server.on("connection", yt.YtLogSocket());
    insecure_server.on("connection", function(socket) {
        socket.setTimeout(5 * 60 * 1000);
        socket.setNoDelay(true);
        socket.setKeepAlive(true);
    });
} else {
    insecure_close_deferred.reject(new Error("Insecure server is not enabled"));
    insecure_listening_deferred.reject(new Error("Insecure server is not enabled"));
}

if (config.ssl_port && config.ssl_address) {
    logger.info("Binding to secure socket", {
        wid: cluster.worker.id,
        pid: process.pid,
        address: config.ssl_address,
        port: config.ssl_port
    });

    var ssl_key = fs.readFileSync(config.ssl_key);
    var ssl_certificate = fs.readFileSync(config.ssl_certificate);
    var ssl_ca = null;
    if (config.ssl_ca) {
        ssl_ca = fs.readFileSync(config.ssl_ca);
    }

    secure_server = https.createServer({
        key: ssl_key,
        passphrase: config.ssl_passphrase,
        cert: ssl_certificate,
        ca: ssl_ca,
        ciphers: config.ssl_ciphers,
        rejectUnauthorized: config.ssl_reject_unauthorized,
    }, application);
    secure_server.listen(config.ssl_port, config.ssl_address);

    secure_server.on("close", secure_close_deferred.resolve.bind(secure_close_deferred));
    secure_server.on("listening", secure_listening_deferred.resolve.bind(secure_listening_deferred));
    secure_server.on("secureConnection", yt.YtLogSocket());
} else {
    secure_close_deferred.reject(new Error("Secure server is not enabled"));
    secure_listening_deferred.reject(new Error("Secure server is not enabled"));
}

// TODO(sandello): Add those checks in |secureConnection|.
// $ssl_client_i_dn == "/DC=ru/DC=yandex/DC=ld/CN=YandexExternalCA";
// $ssl_client_s_dn == "/C=RU/ST=Russia/L=Moscow/O=Yandex/OU=Information Security/CN=agranat.yandex-team.ru/emailAddress=security@yandex-team.ru";

Q
    .settle([ insecure_listening_deferred.promise, secure_listening_deferred.promise ])
    .then(function() {
        logger.info("Worker is up and running", {
            wid : cluster.worker.id,
            pid : process.pid
        });
        process.send({ type : "alive" });
    });

Q
    .settle([ insecure_close_deferred.promise, secure_close_deferred.promise ])
    .then(violentlyDie);
