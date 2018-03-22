package ru.yandex.yt.ytclient.proxy;

import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.function.Function;
import java.util.stream.Collectors;

import com.google.protobuf.ByteString;
import com.google.protobuf.MessageLite;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.bolts.collection.Option;
import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.inside.yt.kosher.impl.ytree.serialization.YTreeBinarySerializer;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.rpcproxy.TReqAbortTransaction;
import ru.yandex.yt.rpcproxy.TReqAlterTable;
import ru.yandex.yt.rpcproxy.TReqCommitTransaction;
import ru.yandex.yt.rpcproxy.TReqConcatenateNodes;
import ru.yandex.yt.rpcproxy.TReqCopyNode;
import ru.yandex.yt.rpcproxy.TReqCreateNode;
import ru.yandex.yt.rpcproxy.TReqExistsNode;
import ru.yandex.yt.rpcproxy.TReqFreezeTable;
import ru.yandex.yt.rpcproxy.TReqGenerateTimestamps;
import ru.yandex.yt.rpcproxy.TReqGetInSyncReplicas;
import ru.yandex.yt.rpcproxy.TReqGetNode;
import ru.yandex.yt.rpcproxy.TReqGetTabletInfos;
import ru.yandex.yt.rpcproxy.TReqLinkNode;
import ru.yandex.yt.rpcproxy.TReqListNode;
import ru.yandex.yt.rpcproxy.TReqLockNode;
import ru.yandex.yt.rpcproxy.TReqLookupRows;
import ru.yandex.yt.rpcproxy.TReqModifyRows;
import ru.yandex.yt.rpcproxy.TReqMountTable;
import ru.yandex.yt.rpcproxy.TReqMoveNode;
import ru.yandex.yt.rpcproxy.TReqPingTransaction;
import ru.yandex.yt.rpcproxy.TReqRemountTable;
import ru.yandex.yt.rpcproxy.TReqRemoveNode;
import ru.yandex.yt.rpcproxy.TReqReshardTable;
import ru.yandex.yt.rpcproxy.TReqSelectRows;
import ru.yandex.yt.rpcproxy.TReqSetNode;
import ru.yandex.yt.rpcproxy.TReqStartTransaction;
import ru.yandex.yt.rpcproxy.TReqTrimTable;
import ru.yandex.yt.rpcproxy.TReqUnfreezeTable;
import ru.yandex.yt.rpcproxy.TReqUnmountTable;
import ru.yandex.yt.rpcproxy.TReqVersionedLookupRows;
import ru.yandex.yt.rpcproxy.TRspAbortTransaction;
import ru.yandex.yt.rpcproxy.TRspAlterTable;
import ru.yandex.yt.rpcproxy.TRspCommitTransaction;
import ru.yandex.yt.rpcproxy.TRspConcatenateNodes;
import ru.yandex.yt.rpcproxy.TRspCopyNode;
import ru.yandex.yt.rpcproxy.TRspCreateNode;
import ru.yandex.yt.rpcproxy.TRspExistsNode;
import ru.yandex.yt.rpcproxy.TRspFreezeTable;
import ru.yandex.yt.rpcproxy.TRspGenerateTimestamps;
import ru.yandex.yt.rpcproxy.TRspGetInSyncReplicas;
import ru.yandex.yt.rpcproxy.TRspGetNode;
import ru.yandex.yt.rpcproxy.TRspGetTabletInfos;
import ru.yandex.yt.rpcproxy.TRspLinkNode;
import ru.yandex.yt.rpcproxy.TRspListNode;
import ru.yandex.yt.rpcproxy.TRspLockNode;
import ru.yandex.yt.rpcproxy.TRspLookupRows;
import ru.yandex.yt.rpcproxy.TRspModifyRows;
import ru.yandex.yt.rpcproxy.TRspMountTable;
import ru.yandex.yt.rpcproxy.TRspMoveNode;
import ru.yandex.yt.rpcproxy.TRspPingTransaction;
import ru.yandex.yt.rpcproxy.TRspRemountTable;
import ru.yandex.yt.rpcproxy.TRspRemoveNode;
import ru.yandex.yt.rpcproxy.TRspReshardTable;
import ru.yandex.yt.rpcproxy.TRspSelectRows;
import ru.yandex.yt.rpcproxy.TRspSetNode;
import ru.yandex.yt.rpcproxy.TRspStartTransaction;
import ru.yandex.yt.rpcproxy.TRspTrimTable;
import ru.yandex.yt.rpcproxy.TRspUnfreezeTable;
import ru.yandex.yt.rpcproxy.TRspUnmountTable;
import ru.yandex.yt.rpcproxy.TRspVersionedLookupRows;
import ru.yandex.yt.ytclient.misc.YtTimestamp;
import ru.yandex.yt.ytclient.proxy.request.AlterTable;
import ru.yandex.yt.ytclient.proxy.request.ConcatenateNodes;
import ru.yandex.yt.ytclient.proxy.request.CopyNode;
import ru.yandex.yt.ytclient.proxy.request.CreateNode;
import ru.yandex.yt.ytclient.proxy.request.ExistsNode;
import ru.yandex.yt.ytclient.proxy.request.FreezeTable;
import ru.yandex.yt.ytclient.proxy.request.GetInSyncReplicas;
import ru.yandex.yt.ytclient.proxy.request.GetNode;
import ru.yandex.yt.ytclient.proxy.request.LinkNode;
import ru.yandex.yt.ytclient.proxy.request.ListNode;
import ru.yandex.yt.ytclient.proxy.request.LockMode;
import ru.yandex.yt.ytclient.proxy.request.LockNode;
import ru.yandex.yt.ytclient.proxy.request.LockNodeResult;
import ru.yandex.yt.ytclient.proxy.request.MoveNode;
import ru.yandex.yt.ytclient.proxy.request.ObjectType;
import ru.yandex.yt.ytclient.proxy.request.RemountTable;
import ru.yandex.yt.ytclient.proxy.request.RemoveNode;
import ru.yandex.yt.ytclient.proxy.request.ReshardTable;
import ru.yandex.yt.ytclient.proxy.request.SetNode;
import ru.yandex.yt.ytclient.proxy.request.TabletInfo;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcClientResponse;
import ru.yandex.yt.ytclient.rpc.RpcOptions;
import ru.yandex.yt.ytclient.rpc.RpcUtil;
import ru.yandex.yt.ytclient.rpc.internal.RpcServiceClient;
import ru.yandex.yt.ytclient.tables.TableSchema;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;
import ru.yandex.yt.ytclient.wire.VersionedRowset;

/**
 * Клиент для высокоуровневой работы с ApiService
 */
public class ApiServiceClient {
    private static final Logger logger = LoggerFactory.getLogger(ApiServiceClient.class);

    private final ApiService service;
    private final Executor heavyExecutor;
    private final Option<RpcClient> rpcClient;
    private final RpcOptions rpcOptions;

    private ApiServiceClient(Option<RpcClient> client, RpcOptions options, ApiService service, Executor heavyExecutor) {
        this.service = Objects.requireNonNull(service);
        this.heavyExecutor = Objects.requireNonNull(heavyExecutor);
        this.rpcClient = Objects.requireNonNull(client);
        this.rpcOptions = options;
    }

    private ApiServiceClient(Option<RpcClient> client, RpcOptions options, ApiService service) {
        this(client, options, service, ForkJoinPool.commonPool());
    }

    public ApiServiceClient(RpcClient client, RpcOptions options) {
        this(Option.of(client), options, client.getService(ApiService.class, options));
    }

    ApiServiceClient(RpcOptions options) {
        this(Option.empty(), options, RpcServiceClient.create(ApiService.class, options));
    }

    public ApiServiceClient(RpcClient client) {
        this(client, new RpcOptions());
    }

    public ApiService getService() {
        return service;
    }

    private YTreeNode parseByteString(ByteString byteString) {
        return YTreeBinarySerializer.deserialize(byteString.newInput());
    }

    public CompletableFuture<ApiServiceTransaction> startTransaction(ApiServiceTransactionOptions options) {
        RpcClientRequestBuilder<TReqStartTransaction.Builder, RpcClientResponse<TRspStartTransaction>> builder =
                service.startTransaction();
        builder.body().setType(options.getType());
        Duration timeout = options.getTimeout();
        if (timeout != null) {
            builder.body().setTimeout(ApiServiceUtil.durationToYtMicros(timeout));
        }
        if (options.getId() != null && !options.getId().isEmpty()) {
            builder.body().setId(RpcUtil.toProto(options.getId()));
        }
        if (options.getParentId() != null && !options.getParentId().isEmpty()) {
            builder.body().setParentId(RpcUtil.toProto(options.getParentId()));
        }
        if (options.getAutoAbort() != null) {
            builder.body().setAutoAbort(options.getAutoAbort());
        }
        if (options.getPing() != null) {
            builder.body().setPing(options.getPing());
        }
        if (options.getPingAncestors() != null) {
            builder.body().setPingAncestors(options.getPingAncestors());
        }
        if (options.getSticky() != null) {
            builder.body().setSticky(options.getSticky());
        }
        if (options.getAtomicity() != null) {
            builder.body().setAtomicity(options.getAtomicity());
        }
        if (options.getDurability() != null) {
            builder.body().setDurability(options.getDurability());
        }
        final boolean ping = builder.body().getPing();
        final boolean pingAncestors = builder.body().getPingAncestors();
        final boolean sticky = builder.body().getSticky();
        final Duration pingPeriod = options.getPingPeriod();

        return RpcUtil.apply(invoke(builder), response -> {
            GUID id = RpcUtil.fromProto(response.body().getId());
            YtTimestamp startTimestamp = YtTimestamp.valueOf(response.body().getStartTimestamp());
            RpcClient sender = response.sender();
            if (rpcClient.isSome(sender)) {
                return new ApiServiceTransaction(this, id, startTimestamp, ping, pingAncestors, sticky, pingPeriod, sender.executor());
            } else {
                return new ApiServiceTransaction(sender, rpcOptions, id, startTimestamp, ping, pingAncestors, sticky, pingPeriod, sender.executor());
            }
        });
    }

    public CompletableFuture<Void> pingTransaction(GUID id, boolean sticky) {
        RpcClientRequestBuilder<TReqPingTransaction.Builder, RpcClientResponse<TRspPingTransaction>> builder =
                service.pingTransaction();
        builder.body().setTransactionId(RpcUtil.toProto(id));
        builder.body().setSticky(sticky);
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> commitTransaction(GUID id, boolean sticky) {
        RpcClientRequestBuilder<TReqCommitTransaction.Builder, RpcClientResponse<TRspCommitTransaction>> builder =
                service.commitTransaction();
        builder.body().setTransactionId(RpcUtil.toProto(id));
        builder.body().setSticky(sticky);
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> abortTransaction(GUID id, boolean sticky) {
        RpcClientRequestBuilder<TReqAbortTransaction.Builder, RpcClientResponse<TRspAbortTransaction>> builder =
                service.abortTransaction();
        builder.body().setTransactionId(RpcUtil.toProto(id));
        builder.body().setSticky(sticky);
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    /* nodes */
    public CompletableFuture<YTreeNode> getNode(GetNode req) {
        RpcClientRequestBuilder<TReqGetNode.Builder, RpcClientResponse<TRspGetNode>> builder = service.getNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> parseByteString(response.body().getValue()));
    }

    public CompletableFuture<YTreeNode> getNode(String path) {
        return getNode(new GetNode(path));
    }

    public CompletableFuture<YTreeNode> listNode(ListNode req) {
        RpcClientRequestBuilder<TReqListNode.Builder, RpcClientResponse<TRspListNode>> builder = service.listNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> parseByteString(response.body().getValue()));
    }

    public CompletableFuture<YTreeNode> listNode(String path) {
        return listNode(new ListNode(path));
    }

    public CompletableFuture<Void> setNode(SetNode req) {
        RpcClientRequestBuilder<TReqSetNode.Builder, RpcClientResponse<TRspSetNode>> builder = service.setNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> setNode(String path, byte[] data) {
        return setNode(new SetNode(path, data));
    }

    public CompletableFuture<Void> setNode(String path, YTreeNode data) {
        return setNode(path, data.toBinary());
    }

    public CompletableFuture<Boolean> existsNode(ExistsNode req) {
        RpcClientRequestBuilder<TReqExistsNode.Builder, RpcClientResponse<TRspExistsNode>> builder = service.existsNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder),
                response -> response.body().getExists());
    }

    public CompletableFuture<Boolean> existsNode(String path) {
        return existsNode(new ExistsNode(path));
    }

    public CompletableFuture<GUID> createNode(CreateNode req) {
        RpcClientRequestBuilder<TReqCreateNode.Builder, RpcClientResponse<TRspCreateNode>> builder = service.createNode();
        req.writeTo(builder.body());

        return RpcUtil.apply(invoke(builder),
                response ->
                        RpcUtil.fromProto(response.body().getNodeId()));
    }

    public CompletableFuture<GUID> createNode(String path, ObjectType type) {
        return createNode(new CreateNode(path, type));
    }

    public CompletableFuture<Void> removeNode(RemoveNode req) {
        RpcClientRequestBuilder<TReqRemoveNode.Builder, RpcClientResponse<TRspRemoveNode>> builder = service.removeNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> removeNode(String path) {
        return removeNode(new RemoveNode(path));
    }

    public CompletableFuture<LockNodeResult> lockNode(LockNode req) {
        RpcClientRequestBuilder<TReqLockNode.Builder, RpcClientResponse<TRspLockNode>> builder = service.lockNode();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> new LockNodeResult(
                RpcUtil.fromProto(response.body().getNodeId()),
                RpcUtil.fromProto(response.body().getLockId())));
    }

    public CompletableFuture<LockNodeResult> lockNode(String path, LockMode mode) {
        return lockNode(new LockNode(path, mode));
    }

    public CompletableFuture<GUID> copyNode(CopyNode req) {
        RpcClientRequestBuilder<TReqCopyNode.Builder, RpcClientResponse<TRspCopyNode>> builder = service.copyNode();
        req.writeTo(builder.body());

        return RpcUtil.apply(invoke(builder),
                response ->
                        RpcUtil.fromProto(response.body().getNodeId()));
    }

    public CompletableFuture<GUID> copyNode(String src, String dst) {
        return copyNode(new CopyNode(src, dst));
    }

    public CompletableFuture<GUID> moveNode(MoveNode req) {
        RpcClientRequestBuilder<TReqMoveNode.Builder, RpcClientResponse<TRspMoveNode>> builder = service.moveNode();
        req.writeTo(builder.body());

        return RpcUtil.apply(invoke(builder),
                response ->
                        RpcUtil.fromProto(response.body().getNodeId()));
    }

    public CompletableFuture<GUID> moveNode(String from, String to) {
        return moveNode(new MoveNode(from, to));
    }

    public CompletableFuture<GUID> linkNode(LinkNode req) {
        RpcClientRequestBuilder<TReqLinkNode.Builder, RpcClientResponse<TRspLinkNode>> builder = service.linkNode();
        req.writeTo(builder.body());

        return RpcUtil.apply(invoke(builder),
                response ->
                        RpcUtil.fromProto(response.body().getNodeId()));
    }

    public CompletableFuture<GUID> linkNode(String src, String dst) {
        return linkNode(new LinkNode(src, dst));
    }

    public CompletableFuture<Void> concatenateNodes(ConcatenateNodes req) {
        RpcClientRequestBuilder<TReqConcatenateNodes.Builder, RpcClientResponse<TRspConcatenateNodes>> builder = service.concatenateNodes();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> concatenateNodes(String [] from, String to) {
        return concatenateNodes(new ConcatenateNodes(from, to));
    }

    /* */
    public CompletableFuture<UnversionedRowset> lookupRows(LookupRowsRequest request, YtTimestamp timestamp) {
        RpcClientRequestBuilder<TReqLookupRows.Builder, RpcClientResponse<TRspLookupRows>> builder =
                service.lookupRows();
        builder.body().setPath(request.getPath());
        builder.body().addAllColumns(request.getLookupColumns());
        if (request.getKeepMissingRows().isPresent()) {
            builder.body().setKeepMissingRows(request.getKeepMissingRows().get());
        }
        if (timestamp != null) {
            builder.body().setTimestamp(timestamp.getValue());
        }
        builder.body().setRowsetDescriptor(ApiServiceUtil.makeRowsetDescriptor(request.getSchema()));
        request.serializeRowsetTo(builder.attachments());
        return handleHeavyResponse(invoke(builder), response -> {
            logger.trace("LookupRows incoming rowset descriptor: {}", response.body().getRowsetDescriptor());
            return ApiServiceUtil
                    .deserializeUnversionedRowset(response.body().getRowsetDescriptor(), response.attachments());
        });
    }

    public CompletableFuture<UnversionedRowset> lookupRows(LookupRowsRequest request) {
        return lookupRows(request, null);
    }

    public CompletableFuture<VersionedRowset> versionedLookupRows(LookupRowsRequest request, YtTimestamp timestamp) {
        RpcClientRequestBuilder<TReqVersionedLookupRows.Builder, RpcClientResponse<TRspVersionedLookupRows>> builder =
                service.versionedLookupRows();
        builder.body().setPath(request.getPath());
        builder.body().addAllColumns(request.getLookupColumns());
        if (request.getKeepMissingRows().isPresent()) {
            builder.body().setKeepMissingRows(request.getKeepMissingRows().get());
        }
        if (timestamp != null) {
            builder.body().setTimestamp(timestamp.getValue());
        }
        builder.body().setRowsetDescriptor(ApiServiceUtil.makeRowsetDescriptor(request.getSchema()));
        request.serializeRowsetTo(builder.attachments());
        return handleHeavyResponse(invoke(builder), response -> {
            logger.trace("VersionedLookupRows incoming rowset descriptor: {}", response.body().getRowsetDescriptor());
            return ApiServiceUtil
                    .deserializeVersionedRowset(response.body().getRowsetDescriptor(), response.attachments());
        });
    }

    public CompletableFuture<VersionedRowset> versionedLookupRows(LookupRowsRequest request) {
        return versionedLookupRows(request, null);
    }

    public CompletableFuture<UnversionedRowset> selectRows(String query) {
        return selectRows(SelectRowsRequest.of(query));
    }

    public CompletableFuture<UnversionedRowset> selectRows(SelectRowsRequest request) {
        RpcClientRequestBuilder<TReqSelectRows.Builder, RpcClientResponse<TRspSelectRows>> builder =
                service.selectRows();
        builder.body().setQuery(request.getQuery());
        if (request.getTimestamp().isPresent()) {
            builder.body().setTimestamp(request.getTimestamp().get().getValue());
        }
        if (request.getInputRowsLimit().isPresent()) {
            builder.body().setInputRowLimit(request.getInputRowsLimit().getAsLong());
        }
        if (request.getOutputRowsLimit().isPresent()) {
            builder.body().setOutputRowLimit(request.getOutputRowsLimit().getAsLong());
        }
        return handleHeavyResponse(invoke(builder), response -> {
            logger.trace("SelectRows incoming rowset descriptor: {}", response.body().getRowsetDescriptor());
            return ApiServiceUtil
                    .deserializeUnversionedRowset(response.body().getRowsetDescriptor(), response.attachments());
        });
    }

    public CompletableFuture<Void> modifyRows(GUID transactionId, ModifyRowsRequest request) {
        RpcClientRequestBuilder<TReqModifyRows.Builder, RpcClientResponse<TRspModifyRows>> builder =
                service.modifyRows();
        builder.body().setTransactionId(RpcUtil.toProto(transactionId));
        builder.body().setPath(request.getPath());
        if (request.getRequireSyncReplica().isPresent()) {
            builder.body().setRequireSyncReplica(request.getRequireSyncReplica().get());
        }
        builder.body().addAllRowModificationTypes(request.getRowModificationTypes());
        builder.body().setRowsetDescriptor(ApiServiceUtil.makeRowsetDescriptor(request.getSchema()));
        request.serializeRowsetTo(builder.attachments());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<List<GUID>> getInSyncReplicas(GetInSyncReplicas request, YtTimestamp timestamp) {
        RpcClientRequestBuilder<TReqGetInSyncReplicas.Builder, RpcClientResponse<TRspGetInSyncReplicas>> builder =
                service.getInSyncReplicas();

        builder.body().setPath(request.getPath());
        builder.body().setTimestamp(timestamp.getValue());
        builder.body().setRowsetDescriptor(ApiServiceUtil.makeRowsetDescriptor(request.getSchema()));

        request.serializeRowsetTo(builder.attachments());

        return RpcUtil.apply(invoke(builder),
                response ->
                        response.body().getReplicaIdsList().stream().map(RpcUtil::fromProto).collect(Collectors.toList()));
    }

    @Deprecated
    public CompletableFuture<List<GUID>> getInSyncReplicas(
            String path,
            YtTimestamp timestamp,
            TableSchema schema,
            Iterable<? extends List<?>> keys)
    {
        return getInSyncReplicas(new GetInSyncReplicas(path, schema, keys), timestamp);
    }

    public CompletableFuture<List<TabletInfo>> getTabletInfos(String path, List<Integer> tabletIndices)
    {
        RpcClientRequestBuilder<TReqGetTabletInfos.Builder, RpcClientResponse<TRspGetTabletInfos>> builder =
                service.getTabletInfos();

        builder.body().setPath(path);
        builder.body().addAllTabletIndexes(tabletIndices);

        return RpcUtil.apply(invoke(builder),
                response ->
                        response.body()
                                .getTabletsList()
                                .stream()
                                .map(x -> new TabletInfo(x.getTotalRowCount(), x.getTrimmedRowCount()))
                .collect(Collectors.toList()));
    }

    public CompletableFuture<YtTimestamp> generateTimestamps(int count) {
        RpcClientRequestBuilder<TReqGenerateTimestamps.Builder, RpcClientResponse<TRspGenerateTimestamps>> builder =
                service.generateTimestamps();

        builder.body().setCount(count);
        return RpcUtil.apply(invoke(builder),
                response -> YtTimestamp.valueOf(response.body().getTimestamp()));
    }

    public CompletableFuture<YtTimestamp> generateTimestamps() {
        return generateTimestamps(1);
    }

    /* tables */
    public CompletableFuture<Void> mountTable(String path, GUID cellId, boolean freeze) {
        RpcClientRequestBuilder<TReqMountTable.Builder, RpcClientResponse<TRspMountTable>> builder =
                service.mountTable();

        builder.body().setPath(path);
        builder.body().setFreeze(freeze);
        if (cellId != null) {
            builder.body().setCellId(RpcUtil.toProto(cellId));
        }
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> mountTable(String path) {
        return mountTable(path, null, false);
    }

    public CompletableFuture<Void> mountTable(String path, boolean freeze) {
        return mountTable(path, null, freeze);
    }

    public CompletableFuture<Void> unmountTable(String path, boolean force) {
        RpcClientRequestBuilder<TReqUnmountTable.Builder, RpcClientResponse<TRspUnmountTable>> builder =
                service.unmountTable();
        builder.body().setPath(path);
        builder.body().setForce(force);
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> unmountTable(String path) {
        return unmountTable(path, false);
    }

    public CompletableFuture<Void> remountTable(RemountTable req) {
        RpcClientRequestBuilder<TReqRemountTable.Builder, RpcClientResponse<TRspRemountTable>> builder =
                service.remountTable();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> remountTable(String path) {
        return remountTable(new RemountTable(path));
    }

    public CompletableFuture<Void> freezeTable(FreezeTable req) {
        RpcClientRequestBuilder<TReqFreezeTable.Builder, RpcClientResponse<TRspFreezeTable>> builder =
                service.freezeTable();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> freezeTable(String path) {
        return freezeTable(new FreezeTable(path));
    }

    public CompletableFuture<Void> unfreezeTable(FreezeTable req) {
        RpcClientRequestBuilder<TReqUnfreezeTable.Builder, RpcClientResponse<TRspUnfreezeTable>> builder =
                service.unfreezeTable();
        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> unfreezeTable(String path) {
        return unfreezeTable(new FreezeTable(path));
    }

    public CompletableFuture<Void> reshardTable(ReshardTable req) {
        RpcClientRequestBuilder<TReqReshardTable.Builder, RpcClientResponse<TRspReshardTable>> builder =
                service.reshardTable();

        req.writeTo(builder.body());

        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> trimTable(String path, int tableIndex, long trimmedRowCount) {
        RpcClientRequestBuilder<TReqTrimTable.Builder, RpcClientResponse<TRspTrimTable>> builder =
                service.trimTable();
        builder.body().setPath(path);
        builder.body().setTabletIndex(tableIndex);
        builder.body().setTrimmedRowCount(trimmedRowCount);
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> alterTable(AlterTable req) {
        RpcClientRequestBuilder<TReqAlterTable.Builder, RpcClientResponse<TRspAlterTable>> builder =
                service.alterTable();

        req.writeTo(builder.body());
        return RpcUtil.apply(invoke(builder), response -> null);
    }

    public CompletableFuture<Void> alterTableReplica() {
        CompletableFuture<Void> r = new CompletableFuture<>();
        r.completeExceptionally(new RuntimeException("unimplemented"));
        return r;
    }
    /* */

    private <T, Response> CompletableFuture<T> handleHeavyResponse(CompletableFuture<Response> future,
                                                                   Function<Response, T> fn) {
        return RpcUtil.applyAsync(future, fn, heavyExecutor);
    }

    protected <RequestType extends MessageLite.Builder, ResponseType> CompletableFuture<ResponseType> invoke(
            RpcClientRequestBuilder<RequestType, ResponseType> builder)
    {
        return builder.invoke();
    }
}
