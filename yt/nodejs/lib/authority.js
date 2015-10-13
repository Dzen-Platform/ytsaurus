var lru_cache = require("lru-cache");
var Q = require("bluebird");

var YtError = require("./error").that;

var external_services = require("./external_services");
var utils = require("./utils");

////////////////////////////////////////////////////////////////////////////////

var __DBG = require("./debug").that("A", "Authentication");

////////////////////////////////////////////////////////////////////////////////

function createNewResult()
{
    // This structure represents a mutable response state.
    // If |result.login| is string, then it is a login.
    // If |result.login| === false, then it is a rejection.
    var result = {
        login: false,
        realm: false,
    };

    Object.defineProperty(result, "isAuthenticated", {
        get: function() {
            return typeof(this.login) === "string" && typeof(this.realm) === "string";
        },
        enumerable: true
    });

    return result;
}

function YtAuthority(config, driver)
{
    this.__DBG = __DBG.Tagged();

    this.config = config;
    this.driver = driver;

    // Caches token authentication results.
    this.authentication_cache = lru_cache({
        max: this.config.cache_max_size,
        maxAge: this.config.cache_max_token_age,
    });

    // Caches user existence.
    this.exist_cache = lru_cache({
        max: this.config.cache_max_size,
        maxAge: this.config.cache_max_exist_age,
    });

    this.__DBG("New");
}

YtAuthority.prototype.authenticateByToken = Q.method(
function YtAuthority$authenticateByToken(logger, profiler, party, token)
{
    this.__DBG("authenticateByToken");

    // This structure represents an immutable request state.
    // It is passed to all subsequent calls.
    var context = {
        ts: new Date(),
        logger: logger,
        profiler: profiler,
        party: party,
        cache_key: "T" + token,
        token: token,
    };

    var result = createNewResult();

    // Fast-path.
    if (this._syncCheckCache(context, result)) {
        return result;
    }

    // Perform proper authentication here and cache the result.
    return Q.resolve()
    .then(this._asyncQueryCypress.bind(this, context, result))
    .then(this._asyncQueryBlackboxToken.bind(this, context, result))
    .then(this._ensureUserExists.bind(this, context, result))
    .then(this._syncFinalize.bind(this, context, result));
});

YtAuthority.prototype.authenticateByCookie = Q.method(
function YtAuthority$authenticateByCookie(logger, profiler, party, sessionid, sslsessionid)
{
    this.__DBG("authenticateByCookie");

    // This structure represents an immutable request state.
    // It is passed to all subsequent calls.
    var context = {
        ts: new Date(),
        logger: logger,
        profiler: profiler,
        party: party,
        cache_key: "C" + sessionid + "/" + sslsessionid,
        sessionid: sessionid,
        sslsessionid: sslsessionid,
    };

    var result = createNewResult();

    // Fast-path.
    if (this._syncCheckCache(context, result)) {
        return result;
    }

    // Perform proper authentication here.
    return Q.resolve()
    .then(this._asyncQueryBlackboxCookie.bind(this, context, result))
    .then(this._ensureUserExists.bind(this, context, result))
    .then(this._syncFinalize.bind(this, context, result));
});

YtAuthority.prototype.oAuthObtainToken = Q.method(
function YtAuthority$oAuthObtainToken(logger, party, key, code)
{
    this.__DBG("oAuthObtainToken");

    var app = this._findOAuthApplicationByKey(key);
    if (typeof(app) === "undefined") {
        throw new YtError(
            "There is no OAuth application with key " +
            JSON.stringify(key) + ".");
    }

    return external_services.oAuthObtainToken(
        logger,
        app.client_id,
        app.client_secret,
        code)
    .then(function(data) {
        if (typeof(data.access_token) === "undefined") {
            throw new YtError("Unreachable (you are lucky)");
        }
        return data.access_token;
    });
});

YtAuthority.prototype.oAuthBuildUrlToRedirect = Q.method(
function YtAuthority$oAuthBuildUrlToRedirect(logger, party, key, state)
{
    this.__DBG("oAuthBuildUrlToRedirect");

    var app = this._findOAuthApplicationByKey(key);
    if (typeof(app) === "undefined") {
        throw new YtError(
            "There is no OAuth application with key " +
            JSON.stringify(key) + ".");
    }

    return external_services.oAuthBuildUrlToRedirect(app.client_id, state);
});

YtAuthority.prototype._findOAuthApplicationBy = function(key, value)
{
    this.__DBG("_findOAuthApplicationBy");

    var apps = this.config.oauth;
    for (var i = 0, n = apps.length; i < n; ++i) {
        if (apps[i][key] === value) {
            return apps[i];
        }
    }
};

YtAuthority.prototype._findOAuthApplicationByKey = function(key)
{
    this.__DBG("_findOAuthApplicationByKey");

    return this._findOAuthApplicationBy(
        "key",
        key || this.config.default_oauth_application_key);
};

YtAuthority.prototype._syncCheckCache = function(context, result)
{
    this.__DBG("_syncCheckCache");

    var cached_result = this.authentication_cache.get(context.cache_key);
    if (typeof(cached_result) !== "undefined") {
        // Since |result| is a "pointer", we have to set fields explicitly.
        // We can't do something like |*result = cached_result;|.
        context.logger.debug("Authentication cache hit", {
            login: cached_result.login,
            realm: cached_result.realm
        });
        result.login = cached_result.login;
        result.realm = cached_result.realm;
        return true;
    } else {
        context.logger.debug("Authentication cache miss");
    }
};

YtAuthority.prototype._asyncQueryBlackboxToken = function(context, result)
{
    this.__DBG("_asyncQueryBlackboxToken");

    if (!this.config.blackbox.enable || result.isAuthenticated) {
        return Q.resolve();
    }

    var self = this;

    return external_services.blackboxValidateToken(
        context.logger,
        context.party,
        context.token)
    .then(function(data) {
        // Since we are caching results, we don't have to bother too much about
        // the most efficient order here. We prefer to keep logic readable.
        if (typeof(data.error) === "undefined") {
            throw new YtError("Unreachable (you are lucky)");
        }

        if (!(data.error === "OK" &&
            typeof(data.oauth) === "object" &&
            typeof(data.login) === "string"))
        {
            context.logger.debug("Blackbox has rejected the token");
            return;
        }

        var scope = data.oauth.scope.split(/\s+/);
        var grant = self.config.blackbox.grant;
        if (scope.indexOf(grant) === -1) {
            context.logger.debug(
                "Token does not provide '" + grant + "' grant");
            return;
        }

        var realm = self._findOAuthApplicationBy("client_id", data.oauth.client_id);
        if (typeof(realm) === "undefined") {
            context.logger.debug("Token was issued by the unknown realm");
            return;
        } else {
            realm = realm.key;
        }

        context.logger.debug("Blackbox has approved the token");

        result.login = data.login;
        result.realm = "blackbox-" + realm;
    });
};

YtAuthority.prototype._asyncQueryBlackboxCookie = function(context, result)
{
    this.__DBG("_asyncQueryBlackboxCookie");

    if (!this.config.blackbox.enable || result.isAuthenticated) {
        return Q.resolve();
    }

    var self = this;

    return external_services.blackboxValidateCookie(
        context.logger,
        context.party,
        context.sessionid,
        context.sslsessionid)
    .then(function(data) {
        // Since we are caching results, we don't have to bother too much about
        // the most efficient order here. We prefer to keep logic readable.
        if (typeof(data.status) === "undefined") {
            throw new YtError("Unreachable (you are lucky)");
        }

        switch (data.status.id) {
            case 0: // VALID
            case 1: // NEED_RESET
                context.logger.debug("Blackbox has approved the cookie");
                result.login = data.login;
                result.realm = "blackbox_session_cookie";
                break;
            /*
            case 2: // EXPIRED
            case 3: // NOAUTH
            case 4: // DISABLED
            case 5: // INVALID
            */
            default:
                context.logger.debug("Blackbox has rejected the cookie");
                break;
        }
    });
};

YtAuthority.prototype._syncFinalize = function(context, result)
{
    this.__DBG("_syncFinalize");

    var dt = (new Date()) - context.ts;
    var success;
    if (result.isAuthenticated) {
        context.logger.debug("Authentication succeeded", {
            authentication_time: dt,
        });
        success = 1;
        this.authentication_cache.set(context.cache_key, result);
    } else {
        context.logger.info("Authentication failed", {
            authentication_time: dt,
        });
        success = 0;
        this.authentication_cache.del(context.cache_key);
    }

    var tags = {
        success: success,
        authenticated_user: result.login,
        authenticated_from: result.realm
    };
    context.profiler.inc("yt.http_proxy.authentication_count", tags, 1);
    context.profiler.upd("yt.http_proxy.authentication_time", tags, dt);

    return result;
};

YtAuthority.prototype._asyncQueryCypress = function(context, result)
{
    this.__DBG("_asyncQueryCypress");

    if (!this.config.cypress.enable || result.isAuthenticated) {
        return Q.resolve();
    }

    var self = this;
    var path = self.config.cypress.where + "/" + utils.escapeYPath(context.token);

    return this.driver.executeSimple("get", {
        path: path
    })
    .then(
    function(login) {
        if (typeof(login) !== "string") {
            context.logger.debug("Encountered garbage at path '" + path + "'");
            return;
        }

        context.logger.debug("Cypress has approved the token");

        result.login = login;
        result.realm = "cypress";
    },
    function(error) {
        if (error.checkFor(500)) {
            return; // Resolve error, return 'undefined';
        } else {
            return Q.reject(error);
        }
    });
};

YtAuthority.prototype._ensureUserExists = function(context, result)
{
    this.__DBG("_ensureUserExists");

    var self = this;
    var name = result.login;

    if (!result.isAuthenticated || this.exist_cache.get(name)) {
        return Q.resolve();
    }

    return this.driver.executeSimple("create", {
        type: "user",
        attributes: { name: name }
    })
    .then(
    function(create) {
        context.logger.debug("User created", { name: name });
        self.exist_cache.set(name, true);
    },
    function(error) {
        if (error.checkFor(501)) {
            context.logger.debug("User already exists", { name: name });
            self.exist_cache.set(name, true);
            return;
        } else {
            return Q.reject(error);
        }
    });
};

////////////////////////////////////////////////////////////////////////////////

exports.that = YtAuthority;
