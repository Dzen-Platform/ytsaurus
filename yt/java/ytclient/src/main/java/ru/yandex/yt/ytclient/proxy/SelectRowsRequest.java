package ru.yandex.yt.ytclient.proxy;

import java.util.Optional;
import java.util.OptionalLong;

import ru.yandex.yt.ytclient.misc.YtTimestamp;

public class SelectRowsRequest {
    private final String query;
    private YtTimestamp timestamp;
    private Long inputRowsLimit;
    private Long outputRowsLimit;
    private Boolean failOnIncompleteResult;

    private SelectRowsRequest(String query) {
        this.query = query;
    }

    public static SelectRowsRequest of(String query) {
        return new SelectRowsRequest(query);
    }

    public String getQuery() {
        return query;
    }

    public SelectRowsRequest setTimestamp(YtTimestamp timestamp) {
        this.timestamp = timestamp;
        return this;
    }

    public Optional<YtTimestamp> getTimestamp() {
        return Optional.ofNullable(timestamp);
    }

    public SelectRowsRequest setInputRowsLimit(long inputRowsLimit) {
        this.inputRowsLimit = inputRowsLimit;
        return this;
    }

    public OptionalLong getInputRowsLimit() {
        return inputRowsLimit == null ? OptionalLong.empty() : OptionalLong.of(inputRowsLimit);
    }

    public SelectRowsRequest setFailOnIncompleteResult(boolean failOnIncompleteResult) {
        this.failOnIncompleteResult = failOnIncompleteResult;
        return this;
    }

    public Optional<Boolean> getFailOnIncompleteResult() {
        return Optional.ofNullable(failOnIncompleteResult);
    }

    public SelectRowsRequest setOutputRowsLimit(long outputRowsLimit) {
        this.outputRowsLimit = outputRowsLimit;
        return this;
    }

    public OptionalLong getOutputRowsLimit() {
        return outputRowsLimit == null ? OptionalLong.empty() : OptionalLong.of(outputRowsLimit);
    }

}
