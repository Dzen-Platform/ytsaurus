package ru.yandex.yt.ytclient.request;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

import javax.annotation.Nullable;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.ERowModificationType;
import ru.yandex.yt.rpcproxy.TReqModifyRows;
import ru.yandex.yt.ytclient.SerializationResolver;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcCompression;
import ru.yandex.yt.ytclient.rpc.RpcUtil;
import ru.yandex.yt.ytclient.rpc.internal.Compression;
import ru.yandex.yt.ytclient.tables.TableSchema;

/**
 * Immutable row modification request that contains serialized and compressed rowset
 *
 * It is useful when same request is performed multiple times (e.g. to different clusters)
 * and saves CPU resources used on serialization and compressing.
 *
 * It should be built using {@link PreparableModifyRowsRequest#prepare(RpcCompression)} method
 *
 * Compression used in this request have to match with compression of the client otherwise exception will be thrown
 * when trying to execute this request.
 */
@NonNullApi
@NonNullFields
public class PreparedModifyRowRequest
        extends AbstractModifyRowsRequest<PreparedModifyRowRequest.Builder, PreparedModifyRowRequest> {
    private final Compression codecId;
    private final List<byte[]> compressedAttachments;

    public PreparedModifyRowRequest(BuilderBase<?> builder) {
        super(builder);
        this.codecId = Objects.requireNonNull(builder.codecId);
        this.compressedAttachments = new ArrayList<>(Objects.requireNonNull(builder.compressedAttachments));
    }

    PreparedModifyRowRequest(
            String path,
            TableSchema schema,
            List<ERowModificationType> rowModificationTypes,
            Compression codecId,
            List<byte[]> compressedAttachments
    ) {
        this(builder()
                .setPath(path)
                .setSchema(schema)
                .setRowModificationTypes(rowModificationTypes)
                .setCodecId(codecId)
                .setCompressedAttachments(compressedAttachments));
    }

    public static Builder builder() {
        return new Builder();
    }

    @Override
    public void serializeRowsetTo(RpcClientRequestBuilder<TReqModifyRows.Builder, ?> builder) {
        builder.setCompressedAttachments(codecId, compressedAttachments);
    }

    @Override
    public void convertValues(SerializationResolver serializationResolver) {
    }

    @Override
    public Builder toBuilder() {
        return builder()
                .setCodecId(codecId)
                .setCompressedAttachments(compressedAttachments)
                .setPath(path)
                .setSchema(schema)
                .setRequireSyncReplica(requireSyncReplica)
                .setRowModificationTypes(rowModificationTypes);
    }

    public static class Builder extends BuilderBase<Builder> {
        @Override
        protected Builder self() {
            return this;
        }

        @Override
        public PreparedModifyRowRequest build() {
            return new PreparedModifyRowRequest(this);
        }
    }

    public abstract static class BuilderBase<TBuilder extends BuilderBase<TBuilder>>
            extends AbstractModifyRowsRequest.Builder<TBuilder, PreparedModifyRowRequest> {
        @Nullable
        private Compression codecId;
        @Nullable
        private List<byte[]> compressedAttachments;

        public TBuilder setCodecId(Compression codecId) {
            this.codecId = codecId;
            return self();
        }

        public TBuilder setCompressedAttachments(List<byte[]> compressedAttachments) {
            this.compressedAttachments = compressedAttachments;
            return self();
        }
    }
}

abstract class PreparableModifyRowsRequest<
        TBuilder extends PreparableModifyRowsRequest.Builder<TBuilder, TRequest>,
        TRequest extends PreparableModifyRowsRequest<TBuilder, TRequest>>
        extends AbstractModifyRowsRequest<TBuilder, TRequest> {
    PreparableModifyRowsRequest(Builder<?, ?> builder) {
        super(builder);
    }

    @Override
    public void serializeRowsetTo(RpcClientRequestBuilder<TReqModifyRows.Builder, ?> builder) {
        serializeRowsetTo(builder.attachments());
    }

    abstract void serializeRowsetTo(List<byte[]> attachments);

    /**
     * Serialize and compress rowset.
     */
    public PreparedModifyRowRequest prepare(
            RpcCompression rpcCompression, SerializationResolver serializationResolver) {
        convertValues(serializationResolver);
        List<byte[]> attachments = new ArrayList<>();
        serializeRowsetTo(attachments);
        Compression codecId = rpcCompression.getRequestCodecId().orElse(Compression.fromValue(0));
        List<byte[]> preparedMessage = RpcUtil.createCompressedAttachments(attachments, codecId);

        return new PreparedModifyRowRequest(path, schema, rowModificationTypes, codecId, preparedMessage);
    }

    public abstract static class Builder<
            TBuilder extends Builder<TBuilder, TRequest>,
            TRequest extends AbstractModifyRowsRequest<?, TRequest>>
            extends AbstractModifyRowsRequest.Builder<TBuilder, TRequest> {
    }
}
