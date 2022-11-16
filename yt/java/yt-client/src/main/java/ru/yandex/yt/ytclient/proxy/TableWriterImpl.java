package ru.yandex.yt.ytclient.proxy;

import java.io.IOException;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;

import javax.annotation.Nullable;

import tech.ytsaurus.client.request.WriteTable;
import tech.ytsaurus.client.rpc.Compression;
import tech.ytsaurus.client.rpc.RpcUtil;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TWriteTableMeta;
import ru.yandex.yt.ytclient.SerializationResolver;
import ru.yandex.yt.ytclient.object.UnversionedRowSerializer;
import ru.yandex.yt.ytclient.tables.TableSchema;
import ru.yandex.yt.ytclient.wire.UnversionedRow;

@NonNullApi
class TableWriterBaseImpl<T> extends RawTableWriterImpl {
    protected @Nullable TableSchema schema;
    protected final WriteTable<T> req;
    protected @Nullable TableRowsSerializer<T> tableRowsSerializer;
    private final SerializationResolver serializationResolver;

    TableWriterBaseImpl(WriteTable<T> req, SerializationResolver serializationResolver) {
        super(req.getWindowSize(), req.getPacketSize());
        this.req = req;
        this.serializationResolver = serializationResolver;
        this.tableRowsSerializer = TableRowsSerializer.createTableRowsSerializer(
                this.req.getSerializationContext(), serializationResolver);
    }

    public CompletableFuture<TableWriterBaseImpl<T>> startUploadImpl() {
        TableWriterBaseImpl<T> self = this;

        return startUpload.thenApply((attachments) -> {
            if (attachments.size() != 1) {
                throw new IllegalArgumentException("protocol error");
            }
            byte[] head = attachments.get(0);
            if (head == null) {
                throw new IllegalArgumentException("protocol error");
            }

            TWriteTableMeta metadata = RpcUtil.parseMessageBodyWithCompression(
                    head,
                    TWriteTableMeta.parser(),
                    Compression.None
            );
            self.schema = ApiServiceUtil.deserializeTableSchema(metadata.getSchema());
            logger.debug("schema -> {}", schema.toYTree().toString());

            if (this.tableRowsSerializer == null) {
                if (!this.req.getSerializationContext().getObjectClazz().isPresent()) {
                    throw new IllegalStateException("No object clazz");
                }
                Class<T> objectClazz = self.req.getSerializationContext().getObjectClazz().get();
                if (UnversionedRow.class.equals(objectClazz)) {
                    this.tableRowsSerializer =
                            (TableRowsSerializer<T>) new TableRowsWireSerializer<>(new UnversionedRowSerializer());
                } else {
                    this.tableRowsSerializer = new TableRowsWireSerializer<>(
                            serializationResolver.createWireRowSerializer(
                                    serializationResolver.forClass(objectClazz, self.schema))
                    );
                }
            }

            return self;
        });
    }

    public boolean write(List<T> rows, TableSchema schema) throws IOException {
        byte[] serializedRows = tableRowsSerializer.serialize(rows, schema);
        return write(serializedRows);
    }
}

@NonNullApi
@NonNullFields
class TableWriterImpl<T> extends TableWriterBaseImpl<T> implements TableWriter<T> {
    TableWriterImpl(WriteTable<T> req, SerializationResolver serializationResolver) {
        super(req, serializationResolver);
    }

    @SuppressWarnings("unchecked")
    public CompletableFuture<TableWriter<T>> startUpload() {
        return startUploadImpl().thenApply(writer -> (TableWriter<T>) writer);
    }

    @Override
    public boolean write(List<T> rows, TableSchema schema) throws IOException {
        return super.write(rows, schema);
    }

    @Override
    public TableSchema getSchema() {
        return tableRowsSerializer.getSchema();
    }

    @Override
    public CompletableFuture<TableSchema> getTableSchema() {
        return CompletableFuture.completedFuture(schema);
    }
}

@NonNullApi
@NonNullFields
class AsyncTableWriterImpl<T> extends TableWriterBaseImpl<T> implements AsyncWriter<T> {
    AsyncTableWriterImpl(WriteTable<T> req, SerializationResolver serializationResolver) {
        super(req, serializationResolver);
    }

    @SuppressWarnings("unchecked")
    public CompletableFuture<AsyncWriter<T>> startUpload() {
        return super.startUploadImpl().thenApply(writer -> (AsyncWriter<T>) writer);
    }

    @Override
    public CompletableFuture<Void> write(List<T> rows) {
        Objects.requireNonNull(tableRowsSerializer);
        TableSchema schema;
        if (req.getTableSchema().isPresent()) {
            schema = req.getTableSchema().get();
        } else if (tableRowsSerializer.getSchema().getColumnsCount() > 0) {
            schema = tableRowsSerializer.getSchema();
        } else {
            schema = this.schema;
        }

        return writeImpl(rows, schema);
    }

    private CompletableFuture<Void> writeImpl(List<T> rows, TableSchema schema) {
        try {
            if (write(rows, schema)) {
                return CompletableFuture.completedFuture(null);
            }
            return readyEvent().thenCompose(unused -> writeImpl(rows, schema));
        } catch (IOException ex) {
            throw new RuntimeException(ex);
        }
    }
}
