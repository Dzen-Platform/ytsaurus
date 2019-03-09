package ru.yandex.yt.ytclient.rpc;

import ru.yandex.yt.ytclient.rpc.internal.Compression;

public class RpcCompression {
    private Compression requestCodecId = null;
    private Compression responseCodecId = null;

    public RpcCompression() { }

    public RpcCompression(Compression codecId) {
        this(codecId, codecId);
    }

    public RpcCompression(
            Compression requestCodecId,
            Compression responseCodecId)
    {
        this.requestCodecId = requestCodecId;
        this.responseCodecId = responseCodecId;
    }

    public Compression getRequestCodecId() {
        return requestCodecId;
    }

    public Compression getResponseCodecId() {
        return responseCodecId;
    }

    public RpcCompression setRequestCodecId(Compression codecId) {
        this.requestCodecId = codecId;
        return this;
    }

    public RpcCompression setResponseCodecId(Compression codecId) {
        this.responseCodecId = codecId;
        return this;
    }

    public boolean isEmpty() {
        return requestCodecId == null
                || responseCodecId == null;
    }
}
