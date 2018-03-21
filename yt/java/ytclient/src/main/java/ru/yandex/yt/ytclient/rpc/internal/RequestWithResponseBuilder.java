package ru.yandex.yt.ytclient.rpc.internal;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;

import com.google.protobuf.MessageLite;

import ru.yandex.bolts.collection.Option;
import ru.yandex.yt.rpc.TRequestHeader;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientResponse;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcMessageParser;
import ru.yandex.yt.ytclient.rpc.RpcOptions;

public class RequestWithResponseBuilder<RequestType extends MessageLite.Builder, ResponseType extends MessageLite> extends RequestBuilderBase<RequestType, RpcClientResponse<ResponseType>> {
    private final RpcMessageParser<ResponseType> parser;

    public RequestWithResponseBuilder(
            Option<RpcClient> clientOpt, TRequestHeader.Builder header, RequestType body,
            RpcMessageParser<ResponseType> parser,
            RpcOptions options)
    {
        super(clientOpt, header, body, options);
        this.parser = parser;
    }

    @Override
    protected RpcClientResponseHandler createHandler(CompletableFuture<RpcClientResponse<ResponseType>> result) {
        return new RpcClientResponseHandler() {
            @Override
            public void onAcknowledgement(RpcClient sender) {
                // there's nothing to do in that case
            }

            @Override
            public void onResponse(RpcClient sender, List<byte[]> attachments) {
                if (!result.isDone()) {
                    if (attachments.size() < 1 || attachments.get(0) == null) {
                        throw new IllegalStateException("Received response without a body");
                    }
                    result.complete(new LazyResponse<ResponseType>(parser, attachments.get(0),
                            new ArrayList<>(attachments.subList(1, attachments.size())), sender));
                }
            }

            @Override
            public void onError(RpcClient sender, Throwable error) {
                result.completeExceptionally(error);
            }
        };
    }
}
