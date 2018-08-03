package ru.yandex.yt.ytclient.rpc.internal;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;

import com.google.protobuf.MessageLite;

import ru.yandex.bolts.collection.Option;
import ru.yandex.bolts.collection.Tuple2;
import ru.yandex.yt.rpc.TRequestHeader;
import ru.yandex.yt.ytclient.proxy.internal.BalancingResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestControl;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcOptions;
import ru.yandex.yt.ytclient.rpc.RpcUtil;

public abstract class RequestBuilderBase<RequestType extends MessageLite.Builder, ResponseType> implements RpcClientRequestBuilder<RequestType, ResponseType> {
    private final Option<RpcClient> clientOpt;
    private final TRequestHeader.Builder header;
    private final RequestType body;
    private final List<byte[]> attachments = new ArrayList<>();
    private boolean requestAck = true;
    private final RpcOptions options;

    protected RequestBuilderBase(Option<RpcClient> clientOpt, TRequestHeader.Builder header, RequestType body, RpcOptions options) {
        this.clientOpt = clientOpt;
        this.header = header;
        this.body = body;
        this.options = options;

        setTimeout(options.getGlobalTimeout());
        setRequestAck(options.getDefaultRequestAck());
    }

    @Override
    public TRequestHeader.Builder header() {
        return header;
    }

    @Override
    public RequestType body() {
        return body;
    }

    @Override
    public List<byte[]> attachments() {
        return attachments;
    }

    @Override
    public List<byte[]> serialize() {
        return RpcUtil.createRequestMessage(header.build(), body.build(), attachments);
    }

    @Override
    public boolean requestAck() {
        return requestAck;
    }

    @Override
    public void setRequestAck(boolean requestAck) {
        this.requestAck = requestAck;
    }

    @Override
    public CompletableFuture<ResponseType> invoke() {
        CompletableFuture<ResponseType> result = new CompletableFuture<>();
        try {
            RpcClientResponseHandler handler = createHandler(result);
            if (clientOpt.isPresent()) {
                RpcClientRequestControl control = clientOpt.get().send(this, handler);
                result.whenComplete((ignoredResult, ignoredException) -> control.cancel());
            } else {
                throw new IllegalStateException("client is not set");
            }
        } catch (Throwable e) {
            result.completeExceptionally(e);
        }
        return result;
    }

    @Override
    public CompletableFuture<ResponseType> invokeVia(List<RpcClient> clients) {
        CompletableFuture<ResponseType> result = new CompletableFuture<>();
        try {
            RpcClientResponseHandler handler = createHandler(result);
            RpcClientRequestControl control = sendVia(handler, clients);
            result.whenComplete((ignoredResult, ignoredException) -> control.cancel());
        } catch (Throwable e) {
            result.completeExceptionally(e);
        }
        return result;
    }

    private RpcClientRequestControl sendVia(RpcClientResponseHandler handler, List<RpcClient> clients)
    {
        CompletableFuture<Tuple2<RpcClient, List<byte[]>>> f = new CompletableFuture<>();

        try {
            if (clients.isEmpty()) {
                throw new IllegalStateException("empty client list");
            }

            BalancingResponseHandler h = new BalancingResponseHandler(
                    clients.get(0).executor(),
                    options.getFailoverPolicy(),
                    options.getGlobalTimeout(),
                    options.getFailoverTimeout(),
                    f,
                    this,
                    clients,
                    options.getResponseMetricsHolder());

            f.whenComplete((result, error) -> {
                h.cancel();
                if (error == null) {
                    handler.onResponse(result.get1(), result.get2());
                } else {
                    handler.onError(error);
                }
            });
        } catch (Throwable e) {
            handler.onError(e);
        }

        return () -> f.cancel(true);
    }

    protected abstract RpcClientResponseHandler createHandler(CompletableFuture<ResponseType> result);

    @Override
    public String toString() {
        return String.format("%s/%s/%s", getService(), getMethod(), getRequestId());
    }
}
