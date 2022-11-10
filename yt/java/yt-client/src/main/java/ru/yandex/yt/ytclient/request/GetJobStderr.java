package ru.yandex.yt.ytclient.request;

import java.util.Objects;

import javax.annotation.Nullable;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TReqGetJobStderr;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcUtil;

@NonNullApi
@NonNullFields
public class GetJobStderr extends OperationReq<GetJobStderr.Builder, GetJobStderr>
        implements HighLevelRequest<TReqGetJobStderr.Builder> {
    private final GUID jobId;

    GetJobStderr(Builder builder) {
        super(builder);
        this.jobId = Objects.requireNonNull(builder.jobId);
    }

    public GetJobStderr(GUID operationId, GUID jobId) {
        this(builder().setOperationId(operationId).setJobId(jobId));
    }

    public GetJobStderr(String operationAlias, GUID jobId) {
        this(builder().setOperationAlias(operationAlias).setJobId(jobId));
    }

    public static Builder builder() {
        return new Builder();
    }

    @Override
    public Builder toBuilder() {
        return builder()
                .setOperationId(operationId)
                .setOperationAlias(operationAlias)
                .setJobId(jobId)
                .setTimeout(timeout)
                .setRequestId(requestId)
                .setUserAgent(userAgent)
                .setTraceId(traceId, traceSampled)
                .setAdditionalData(additionalData);
    }

    /**
     * Internal method: prepare request to send over network.
     */
    @Override
    public void writeTo(RpcClientRequestBuilder<TReqGetJobStderr.Builder, ?> builder) {
        TReqGetJobStderr.Builder messageBuilder = builder.body();
        writeOperationDescriptionToProto(messageBuilder::setOperationId, messageBuilder::setOperationAlias);
        messageBuilder.setJobId(RpcUtil.toProto(jobId));
    }

    @Override
    protected void writeArgumentsLogString(StringBuilder sb) {
        sb.append("JobId: ").append(jobId).append("; ");
        super.writeArgumentsLogString(sb);
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends OperationReq.Builder<Builder, GetJobStderr> {
        @Nullable
        private GUID jobId;

        Builder() {
        }

        Builder setJobId(GUID jobId) {
            this.jobId = jobId;
            return self();
        }

        public GetJobStderr build() {
            return new GetJobStderr(this);
        }

        @Override
        protected Builder self() {
            return this;
        }
    }
}
