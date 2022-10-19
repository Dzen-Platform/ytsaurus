package ru.yandex.yt.ytclient.request;

import javax.annotation.Nullable;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TFetcherConfig;

@NonNullApi
@NonNullFields
public class FetcherConfig {
    @Nullable
    private final Long nodeRpcTimeout;

    FetcherConfig(Builder builder) {
        this.nodeRpcTimeout = builder.nodeRpcTimeout;
    }

    public static Builder builder() {
        return new Builder();
    }

    public TFetcherConfig.Builder writeTo(TFetcherConfig.Builder builder) {
        if (nodeRpcTimeout != null) {
            builder.setNodeRpcTimeout(nodeRpcTimeout);
        }
        return builder;
    }

    public static class Builder {
        @Nullable
        private Long nodeRpcTimeout;

        public Builder setNodeRpcTimeout(@Nullable Long nodeRpcTimeout) {
            this.nodeRpcTimeout = nodeRpcTimeout;
            return this;
        }

        public FetcherConfig build() {
            return new FetcherConfig(this);
        }
    }
}
