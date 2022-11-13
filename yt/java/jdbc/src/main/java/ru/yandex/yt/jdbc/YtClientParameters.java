package ru.yandex.yt.jdbc;

import java.sql.DriverPropertyInfo;
import java.util.Objects;
import java.util.Properties;
import java.util.function.Function;
import java.util.stream.Stream;


import tech.ytsaurus.client.rpc.Compression;
public enum YtClientParameters implements Function<Properties, DriverPropertyInfo> {
    USERNAME("username"),
    TOKEN("token"),
    COMPRESSION("compression", Compression.None.name(),
            Stream.of(Compression.values()).map(Enum::name).toArray(String[]::new)),
    DEBUG_OUTPUT("debug_output", "false", new String[]{"true", "false"}),
    MAX_INPUT_LIMIT("max_input_limit", "10000000", null),
    HOME("home"),
    SCAN_RECURSIVE("scan_recursive", "true", new String[]{"true", "false"}),
    ALLOW_JOIN_WITHOUT_INDEX("allowJoinWithoutIndex", "false", new String[]{"true", "false"}),
    UDF_REGISTRY_PATH("udf_registry_path");

    private final String key;
    private final String defaultValue;
    private final String[] choices;

    YtClientParameters(String key) {
        this(key, null, null);
    }

    YtClientParameters(String key, String defaultValue, String[] choices) {
        this.key = Objects.requireNonNull(key);
        this.defaultValue = defaultValue;
        this.choices = choices;
    }

    public String getKey() {
        return key;
    }

    @Override
    public DriverPropertyInfo apply(Properties properties) {
        final DriverPropertyInfo info = new DriverPropertyInfo(key, properties.getProperty(key,
                defaultValue));
        info.required = defaultValue != null;
        info.choices = choices;

        return info;
    }

    String readValue(Properties properties) {
        return properties.getProperty(key, defaultValue);
    }
}
