package ru.yandex.yt.ytclient.proxy.request;

import java.io.ByteArrayOutputStream;

import javax.annotation.Nonnull;
import javax.annotation.Nullable;

import com.google.protobuf.ByteString;

import ru.yandex.inside.yt.kosher.cypress.YPath;
import ru.yandex.inside.yt.kosher.impl.ytree.YTreeBinarySerializer;
import ru.yandex.inside.yt.kosher.impl.ytree.object.YTreeSerializer;
import ru.yandex.inside.yt.kosher.impl.ytree.object.serializers.YTreeObjectSerializer;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.rpcproxy.ERowsetFormat;
import ru.yandex.yt.rpcproxy.TReqReadTable;
import ru.yandex.yt.rpcproxy.TTransactionalOptions;
import ru.yandex.yt.ytclient.object.MappedRowsetDeserializer;
import ru.yandex.yt.ytclient.object.WireRowDeserializer;
import ru.yandex.yt.ytclient.object.YTreeDeserializer;

public class ReadTable<T> extends RequestBase<ReadTable<T>> {
    private final YPath path;
    private final String stringPath;
    @Nullable private final WireRowDeserializer<T> deserializer;
    @Nullable
    private final Class<T> objectClazz;

    private boolean unordered = false;
    private boolean omitInaccessibleColumns = false;
    private YTreeNode config = null;
    private ERowsetFormat desiredRowsetFormat = ERowsetFormat.RF_YT_WIRE;
    private Format format = null;

    private TransactionalOptions transactionalOptions = null;

    public ReadTable(YPath path, WireRowDeserializer<T> deserializer) {
        this.path = path;
        this.stringPath = null;
        this.deserializer = deserializer;
        this.objectClazz = null;
    }

    public ReadTable(YPath path, YTreeObjectSerializer<T> serializer) {
        this.path = path;
        this.stringPath = null;
        this.deserializer = MappedRowsetDeserializer.forClass(serializer);
        this.objectClazz = null;
    }

    public ReadTable(YPath path, YTreeSerializer<T> serializer) {
        this.path = path;
        this.stringPath = null;
        if (serializer instanceof YTreeObjectSerializer) {
            this.deserializer = MappedRowsetDeserializer.forClass((YTreeObjectSerializer<T>) serializer);
        } else {
            this.deserializer = new YTreeDeserializer<>(serializer);
        }
        this.objectClazz = null;
    }

    public ReadTable(YPath path, Class<T> objectClazz) {
        this.path = path;
        this.stringPath = null;
        this.deserializer = null;
        this.objectClazz = objectClazz;
    }

    /**
     * @deprecated Use {@link #ReadTable(YPath path, WireRowDeserializer<T> deserializer)} instead.
     */
    @Deprecated
    public ReadTable(String path, WireRowDeserializer<T> deserializer) {
        this.stringPath = path;
        this.path = null;
        this.deserializer = deserializer;
        this.objectClazz = null;
    }

    /**
     * @deprecated Use {@link #ReadTable(YPath path, YTreeObjectSerializer<T> serializer)} instead.
     */
    @Deprecated
    public ReadTable(String path, YTreeObjectSerializer<T> serializer) {
        this.stringPath = path;
        this.path = null;
        this.deserializer = MappedRowsetDeserializer.forClass(serializer);
        this.objectClazz = null;
    }

    /**
     * @deprecated Use {@link #ReadTable(YPath path,  YTreeSerializer<T> serializer)} instead.
     */
    @Deprecated
    public ReadTable(String path, YTreeSerializer<T> serializer) {
        this.stringPath = path;
        this.path = null;
        if (serializer instanceof YTreeObjectSerializer) {
            this.deserializer = MappedRowsetDeserializer.forClass((YTreeObjectSerializer<T>) serializer);
        } else {
            this.deserializer = new YTreeDeserializer<>(serializer);
        }
        this.objectClazz = null;
    }

    public WireRowDeserializer<T> getDeserializer() {
        return this.deserializer;
    }

    public Class<T> getObjectClazz() {
        return this.objectClazz;
    }

    public ReadTable<T> setTransactionalOptions(TransactionalOptions to) {
        this.transactionalOptions = to;
        return this;
    }

    public ReadTable<T> setUnordered(boolean flag) {
        this.unordered = flag;
        return this;
    }

    public ReadTable<T> setOmitInaccessibleColumns(boolean flag) {
        this.omitInaccessibleColumns = flag;
        return this;
    }

    public ReadTable<T> setConfig(YTreeNode config) {
        this.config = config;
        return this;
    }

    public ReadTable<T> setFormat(Format format) {
        this.format = format;
        this.desiredRowsetFormat = ERowsetFormat.RF_FORMAT;
        return this;
    }

    public ReadTable<T> setDesiredRowsetFormat(ERowsetFormat desiredRowsetFormat) {
        this.desiredRowsetFormat = desiredRowsetFormat;
        return this;
    }

    private String getPath() {
        return path != null ? path.toString() : stringPath;
    }

    public TReqReadTable.Builder writeTo(TReqReadTable.Builder builder) {
        builder.setUnordered(unordered);
        builder.setOmitInaccessibleColumns(omitInaccessibleColumns);
        builder.setPath(getPath());
        if (config != null) {
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            YTreeBinarySerializer.serialize(config, baos);
            byte[] data = baos.toByteArray();
            builder.setConfig(ByteString.copyFrom(data));
        }
        if (format != null) {
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            YTreeBinarySerializer.serialize(format.toTree(), baos);
            byte[] data = baos.toByteArray();
            builder.setFormat(ByteString.copyFrom(data));
        }
        if (transactionalOptions != null) {
            builder.setTransactionalOptions(transactionalOptions.writeTo(TTransactionalOptions.newBuilder()));
        }
        if (additionalData != null) {
            builder.mergeFrom(additionalData);
        }
        builder.setDesiredRowsetFormat(desiredRowsetFormat);
        if (desiredRowsetFormat == ERowsetFormat.RF_FORMAT) {
            if (format == null) {
                throw new IllegalStateException("`format` is required for desiredRowsetFormat == RF_FORMAT");
            }
        }
        return builder;
    }

    @Nonnull
    @Override
    protected ReadTable<T> self() {
        return this;
    }

    @Override
    public ReadTable<T> build() {
        throw new RuntimeException("unimplemented build() method");
    }
}
