package tech.ytsaurus.client;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ScheduledExecutorService;
import java.util.function.Consumer;
import java.util.stream.Collectors;

import javax.annotation.Nullable;

import com.google.protobuf.MessageLite;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import tech.ytsaurus.client.rpc.DataCenterMetricsHolderImpl;
import tech.ytsaurus.client.rpc.RpcClient;
import tech.ytsaurus.client.rpc.RpcClientPool;
import tech.ytsaurus.client.rpc.RpcClientRequestBuilder;
import tech.ytsaurus.client.rpc.RpcClientResponse;
import tech.ytsaurus.client.rpc.RpcClientStreamControl;
import tech.ytsaurus.client.rpc.RpcCompression;
import tech.ytsaurus.client.rpc.RpcCredentials;
import tech.ytsaurus.client.rpc.RpcOptions;
import tech.ytsaurus.client.rpc.RpcStreamConsumer;
import tech.ytsaurus.client.rpc.RpcUtil;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpc.TResponseHeader;
import ru.yandex.yt.rpc.TStreamingFeedbackHeader;
import ru.yandex.yt.rpc.TStreamingPayloadHeader;
import ru.yandex.yt.ytclient.DefaultSerializationResolver;
import ru.yandex.yt.ytclient.SerializationResolver;
import ru.yandex.yt.ytclient.YtClientConfiguration;
import ru.yandex.yt.ytclient.bus.BusConnector;
import ru.yandex.yt.ytclient.bus.DefaultBusConnector;

/**
 *  Asynchronous YT client.
 *  <p>
 *      <b>WARNING</b> Callbacks that <b>can block</b> (e.g. they use {@link CompletableFuture#join})
 *      <b>MUST NEVER BE USED</b> with non-Async (thenApply, whenComplete etc.) methods
 *      called on futures returned by this client. Otherwise, deadlock may appear.
 *      Always use Async versions of these methods with blocking callbacks.
 *  <p>
 *      Explanation. When using non-async thenApply callback is invoked by the thread that sets the future.
 *      In our case it is internal thread of YtClient.
 *      When all internal threads of YtClient are blocked by such callbacks
 *      YtClient becomes unable to send requests and receive responses.
 */
public class YtClientOpensource extends CompoundClientImpl implements BaseYtClient {
    private final BusConnector busConnector;
    private final boolean isBusConnectorOwner;
    private final ScheduledExecutorService executor;
    private final ClientPoolProvider poolProvider;

    private final List<YtCluster> clusters;

    /**
     * @deprecated prefer to use {@link #builder()}
     */
    @Deprecated
    public YtClientOpensource(
            BusConnector connector,
            List<YtCluster> clusters,
            String localDataCenterName,
            RpcCredentials credentials,
            RpcOptions options) {
        this(connector, clusters, localDataCenterName, null, credentials, options);
    }

    /**
     * @deprecated prefer to use {@link #builder()}
     */
    @Deprecated
    public YtClientOpensource(
            BusConnector connector,
            List<YtCluster> clusters,
            String localDataCenterName,
            String proxyRole,
            RpcCredentials credentials,
            RpcOptions options) {
        this(connector, clusters, localDataCenterName, proxyRole, credentials, new RpcCompression(), options);
    }

    /**
     * @deprecated prefer to use {@link #builder()}
     */
    @Deprecated
    public YtClientOpensource(
            BusConnector connector,
            List<YtCluster> clusters,
            String localDataCenterName,
            String proxyRole,
            RpcCredentials credentials,
            RpcCompression compression,
            RpcOptions options
    ) {
        this(
                connector,
                clusters,
                localDataCenterName,
                proxyRole,
                credentials,
                compression,
                options,
                DefaultSerializationResolver.getInstance()
        );
    }

    /**
     * @deprecated prefer to use {@link #builder()}
     */
    @Deprecated
    @SuppressWarnings("checkstyle:ParameterNumber")
    public YtClientOpensource(
            BusConnector connector,
            List<YtCluster> clusters,
            String localDataCenterName,
            String proxyRole,
            RpcCredentials credentials,
            RpcCompression compression,
            RpcOptions options,
            SerializationResolver serializationResolver
    ) {
        this(
                new BuilderWithDefaults<>(
                    new Builder()
                            .setSharedBusConnector(connector)
                            .setClusters(clusters)
                            .setPreferredClusterName(localDataCenterName)
                            .setProxyRole(proxyRole)
                            .setRpcCredentials(credentials)
                            .setRpcCompression(compression)
                            .setRpcOptions(options)
                            .disableValidation()
                            // old constructors did not validate their arguments
                    ), serializationResolver
        );
    }

    public YtClientOpensource(
            BusConnector connector, YtCluster cluster, RpcCredentials credentials, RpcOptions options) {
        this(connector, Arrays.asList(cluster), cluster.getName(), credentials, options);
    }

    public YtClientOpensource(
            BusConnector connector, String clusterName, RpcCredentials credentials, RpcOptions options) {
        this(connector, new YtCluster(clusterName), credentials, options);
    }

    public YtClientOpensource(BusConnector connector, String clusterName, RpcCredentials credentials) {
        this(connector, clusterName, credentials, new RpcOptions());
    }

    protected YtClientOpensource(BuilderWithDefaults builder, SerializationResolver serializationResolver) {
        super(
                builder.busConnector.executorService(), builder.builder.configuration, builder.builder.heavyExecutor,
                serializationResolver
        );

        builder.builder.validate();

        this.busConnector = builder.busConnector;
        this.isBusConnectorOwner = builder.builder.isBusConnectorOwner;
        this.executor = busConnector.executorService();
        this.clusters = builder.builder.clusters;

        OutageController outageController =
                builder.builder.configuration.getRpcOptions().getTestingOptions().getOutageController();

        final RpcClientFactory rpcClientFactory =
                outageController != null
                        ? new OutageRpcClientFactoryImpl(
                                busConnector, builder.credentials, builder.builder.compression,
                                outageController)
                        : new RpcClientFactoryImpl(
                            busConnector,
                            builder.credentials,
                            builder.builder.compression);

        this.poolProvider = new ClientPoolProvider(
                busConnector,
                builder.builder.clusters,
                builder.builder.preferredClusterName,
                builder.builder.proxyRole,
                builder.credentials,
                rpcClientFactory,
                builder.builder.configuration.getRpcOptions(),
                builder.builder.heavyExecutor);
    }

    /**
     * Create builder for YtClient.
     */
    public static ClientBuilder<?, ?> builder() {
        return new Builder();
    }

    public List<YtCluster> getClusters() {
        return clusters;
    }

    public ScheduledExecutorService getExecutor() {
        return executor;
    }

    public CompletableFuture<Void> waitProxies() {
        return poolProvider.waitProxies();
    }

    @Override
    public void close() {
        poolProvider.close();
        if (isBusConnectorOwner) {
            busConnector.close();
        }
        super.close();
    }

    public Map<String, List<ApiServiceClient>> getAliveDestinations() {
        return poolProvider.getAliveDestinations();
    }

    public RpcClientPool getClientPool() {
        return poolProvider.getClientPool();
    }

    @Deprecated
    List<RpcClient> selectDestinations() {
        return poolProvider.oldSelectDestinations();
    }

    /**
     * Method useful in tests. Allows to asynchronously ban client.
     *
     * @return future on number of banned proxies.
     */
    CompletableFuture<Void> banProxy(String address) {
        return poolProvider.banClient(address).thenApply((bannedCount) -> {
            if (bannedCount == 0) {
                throw new RuntimeException("Cannot ban proxy " + address + " since it is not known");
            }
            return null;
        });
    }

    @Override
    protected <RequestType extends MessageLite.Builder, ResponseType extends MessageLite>
    CompletableFuture<RpcClientResponse<ResponseType>> invoke(
            RpcClientRequestBuilder<RequestType, ResponseType> builder
    ) {
        return builder.invokeVia(executor, poolProvider.getClientPool());
    }

    @Override
    protected <RequestType extends MessageLite.Builder, ResponseType extends MessageLite>
    CompletableFuture<RpcClientStreamControl> startStream(
            RpcClientRequestBuilder<RequestType, ResponseType> builder,
            RpcStreamConsumer consumer
    ) {
        CompletableFuture<Void> clientReleaseFuture = new CompletableFuture<>();
        RpcStreamConsumer wrappedConsumer = new RpcStreamConsumer() {
            @Override
            public void onStartStream(RpcClientStreamControl control) {
                consumer.onStartStream(control);
            }

            @Override
            public void onFeedback(RpcClient sender, TStreamingFeedbackHeader header, List<byte[]> attachments) {
                consumer.onFeedback(sender, header, attachments);
            }

            @Override
            public void onPayload(RpcClient sender, TStreamingPayloadHeader header, List<byte[]> attachments) {
                consumer.onPayload(sender, header, attachments);
            }

            @Override
            public void onResponse(RpcClient sender, TResponseHeader header, List<byte[]> attachments) {
                consumer.onResponse(sender, header, attachments);
                clientReleaseFuture.complete(null);
            }

            @Override
            public void onError(Throwable cause) {
                consumer.onError(cause);
                clientReleaseFuture.complete(null);
            }

            @Override
            public void onCancel(CancellationException cancel) {
                consumer.onCancel(cancel);
                clientReleaseFuture.complete(null);
            }

            @Override
            public void onWakeup() {
                consumer.onWakeup();
            }
        };
        CompletableFuture<RpcClient> clientFuture = getClientPool().peekClient(clientReleaseFuture);
        return clientFuture.thenApply(
                client -> client.startStream(client, builder.getRpcRequest(), wrappedConsumer, builder.getOptions())
        );
    }

    @NonNullApi
    static class ClientPoolProvider implements AutoCloseable {
        final MultiDcClientPool multiDcClientPool;
        final List<ClientPoolService> dataCenterList = new ArrayList<>();
        final String localDcName;
        final RpcOptions options;
        final Executor heavyExecutor;
        final ScheduledExecutorService executorService;

        @SuppressWarnings("checkstyle:ParameterNumber")
        ClientPoolProvider(
            BusConnector connector,
            List<YtCluster> clusters,
            @Nullable String localDataCenterName,
            @Nullable String proxyRole,
            RpcCredentials credentials,
            RpcClientFactory rpcClientFactory,
            RpcOptions options,
            Executor heavyExecutor
        ) {
            this.options = options;
            this.localDcName = localDataCenterName;
            this.heavyExecutor = heavyExecutor;
            this.executorService = connector.executorService();

            final EventLoopGroup eventLoopGroup = connector.eventLoopGroup();
            final Random random = new Random();

            for (YtCluster curCluster : clusters) {
                // 1. Понять http-discovery или rpc
                if (curCluster.balancerFqdn != null && !curCluster.balancerFqdn.isEmpty() && (
                        options.getPreferableDiscoveryMethod() == DiscoveryMethod.HTTP
                                || curCluster.addresses == null
                                || curCluster.addresses.isEmpty())
                ) {
                    // Use http
                    dataCenterList.add(
                            ClientPoolService.httpBuilder()
                                    .setDataCenterName(curCluster.getName())
                                    .setBalancerAddress(curCluster.balancerFqdn, curCluster.httpPort)
                                    .setRole(proxyRole)
                                    .setToken(credentials.getToken())
                                    .setOptions(options)
                                    .setClientFactory(rpcClientFactory)
                                    .setEventLoop(eventLoopGroup)
                                    .setRandom(random)
                                    .build()
                    );
                } else if (
                        curCluster.addresses != null && !curCluster.addresses.isEmpty() && (
                        options.getPreferableDiscoveryMethod() == DiscoveryMethod.HTTP
                        || curCluster.balancerFqdn == null || curCluster.balancerFqdn.isEmpty())
                ) {
                    // use rpc
                    List<HostPort> initialProxyList =
                            curCluster.addresses.stream().map(HostPort::parse).collect(Collectors.toList());
                    dataCenterList.add(
                            ClientPoolService.rpcBuilder()
                                    .setDataCenterName(curCluster.getName())
                                    .setInitialProxyList(initialProxyList)
                                    .setRole(proxyRole)
                                    .setOptions(options)
                                    .setClientFactory(rpcClientFactory)
                                    .setEventLoop(eventLoopGroup)
                                    .setRandom(random)
                                    .build()
                    );
                } else {
                    throw new RuntimeException(String.format(
                            "Cluster %s does not have neither http balancer nor rpc proxies specified ",
                            curCluster.getName()
                    ));
                }
            }

            for (ClientPoolService clientPoolService : dataCenterList) {
                clientPoolService.start();
            }

            multiDcClientPool = MultiDcClientPool.builder()
                    .setLocalDc(localDataCenterName)
                    .addClientPools(dataCenterList)
                    .setDcMetricHolder(DataCenterMetricsHolderImpl.INSTANCE)
                    .build();
        }

        @Override
        public void close() {
            for (ClientPoolService dataCenter : dataCenterList) {
                dataCenter.close();
            }
        }

        public CompletableFuture<Void> waitProxies() {
            CompletableFuture<Void> result = new CompletableFuture<>();
            CompletableFuture<RpcClient> client = multiDcClientPool.peekClient(result);
            client.whenComplete((c, error) -> {
                if (error != null) {
                    result.completeExceptionally(error);
                } else {
                    result.complete(null);
                }
            });
            RpcUtil.relayCancel(result, client);
            return result;
        }

        public Map<String, List<ApiServiceClient>> getAliveDestinations() {
            Map<String, List<ApiServiceClient>> result = new HashMap<>();

            CompletableFuture<Void> releaseFuture = new CompletableFuture<>();
            try {
                for (ClientPoolService clientPoolService : dataCenterList) {
                    RpcClient[] aliveClients = clientPoolService.getAliveClients();
                    if (aliveClients.length == 0) {
                        continue;
                    }
                    List<ApiServiceClient> clients =
                            result.computeIfAbsent(clientPoolService.getDataCenterName(), k -> new ArrayList<>());
                    for (RpcClient curClient : aliveClients) {
                        clients.add(new ApiServiceClientImpl(
                                curClient,
                                YtClientConfiguration.builder().setRpcOptions(options).build(),
                                heavyExecutor,
                                executorService,
                                DefaultSerializationResolver.getInstance()
                        ));
                    }
                }
            } finally {
                releaseFuture.complete(null);
            }
            return result;
        }

        public List<RpcClient> oldSelectDestinations() {
            final int resultSize = 3;
            List<RpcClient> result = new ArrayList<>();

            Consumer<ClientPoolService> processClientPoolService = (ClientPoolService service) -> {
                RpcClient[] aliveClients = service.getAliveClients();
                for (RpcClient c : aliveClients) {
                    if (result.size() >= resultSize) {
                        break;
                    }
                    result.add(c);
                }
            };

            for (ClientPoolService clientPoolService : dataCenterList) {
                if (clientPoolService.getDataCenterName().equals(localDcName)) {
                    processClientPoolService.accept(clientPoolService);
                }
            }

            for (ClientPoolService clientPoolService : dataCenterList) {
                if (!clientPoolService.getDataCenterName().equals(localDcName)) {
                    processClientPoolService.accept(clientPoolService);
                }
            }
            return result;
        }

        public RpcClientPool getClientPool() {
            return new NonRepeatingClientPool(multiDcClientPool);
        }

        public CompletableFuture<Integer> banClient(String address) {
            return multiDcClientPool.banClient(address);
        }
    }

    @NonNullApi
    @NonNullFields
    public abstract static class BaseBuilder<
            TClient, TBuilder extends BaseBuilder<TClient, TBuilder>> {
        @Nullable
        RpcCredentials credentials;
        RpcCompression compression = new RpcCompression();
        YtClientConfiguration configuration = YtClientConfiguration.builder()
                .setRpcOptions(new RpcOptions())
                .build();

        /**
         * Set authentication information i.e. username and user token.
         *
         * <p>
         * When no rpc credentials is set they are loaded from environment.
         * @see RpcCredentials#loadFromEnvironment()
         */
        public TBuilder setRpcCredentials(RpcCredentials rpcCredentials) {
            this.credentials = rpcCredentials;
            return self();
        }

        /**
         * Set compression to be used for requests and responses.
         * <p>
         * If it's not specified then no compression is used.
         */
        public TBuilder setRpcCompression(RpcCompression rpcCompression) {
            this.compression = rpcCompression;
            return self();
        }

        /**
         * Set miscellaneous options.
         * Part of YtClientConfiguration.
         * @deprecated prefer to use {@link #setYtClientConfiguration(YtClientConfiguration)} ()}
         */
        @Deprecated
        public TBuilder setRpcOptions(RpcOptions rpcOptions) {
            this.configuration = YtClientConfiguration.builder()
                    .setRpcOptions(rpcOptions)
                    .build();
            return self();
        }

        /**
         * Set settings of YtClient.
         */
        public TBuilder setYtClientConfiguration(YtClientConfiguration configuration) {
            this.configuration = configuration;
            return self();
        }

        protected abstract TBuilder self();

        /**
         * Finally create a client.
         */
        public abstract TClient build();
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends ClientBuilder<YtClientOpensource, Builder> {
        @Override
        protected Builder self() {
            return this;
        }

        @Override
        public YtClientOpensource build() {
            return new YtClientOpensource(new BuilderWithDefaults<>(this), DefaultSerializationResolver.getInstance());
        }
    }

    @NonNullFields
    @NonNullApi
    public abstract static class ClientBuilder<
            TClient extends YtClientOpensource,
            TBuilder extends ClientBuilder<TClient, TBuilder>>
            extends BaseBuilder<TClient, TBuilder> {
        @Nullable BusConnector busConnector;
        boolean isBusConnectorOwner = true;
        @Nullable String preferredClusterName;
        @Nullable String proxyRole;
        List<YtCluster> clusters = new ArrayList<>();

        boolean enableValidation = true;
        Executor heavyExecutor = ForkJoinPool.commonPool();

        ClientBuilder() {
        }

        /**
         * Set YT cluster to use.
         *
         * <p>
         * Similar to {@link #setCluster(String)} but allows to create connections to several clusters.
         * YtClient will choose cluster to send requests based on cluster availability and their ping.
         */
        public TBuilder setClusters(String firstCluster, String... rest) {
            List<YtCluster> ytClusters = new ArrayList<>();
            ytClusters.add(new YtCluster(YtCluster.normalizeName(firstCluster)));
            for (String clusterName : rest) {
                ytClusters.add(new YtCluster(YtCluster.normalizeName(clusterName)));
            }
            return setClusters(ytClusters);
        }

        /**
         * Set YT clusters to use.
         *
         * <p>
         * Similar to {@link #setClusters(String, String...)} but allows finer configuration.
         */
        public TBuilder setClusters(List<YtCluster> clusters) {
            this.clusters = clusters;
            return self();
        }

        /**
         * Set BusConnector for YT client.
         *
         * <p>
         * Connector will be owned by YtClient.
         * YtClient will close it when {@link YtClientOpensource#close()} is called.
         *
         * <p>
         * If bus is never set default bus will be created
         * (default bus will be owned by YtClient, so you don't need to worry about closing it).
         */
        public TBuilder setOwnBusConnector(BusConnector connector) {
            this.busConnector = connector;
            isBusConnectorOwner = true;
            return self();
        }

        /**
         * Set BusConnector for YT client.
         *
         * <p>
         * Connector will not be owned by YtClient. It's user responsibility to close the connector.
         *
         * @see #setOwnBusConnector
         */
        public TBuilder setSharedBusConnector(BusConnector connector) {
            this.busConnector = connector;
            isBusConnectorOwner = false;
            return self();
        }

        /**
         * Set YT cluster to use.
         *
         * @param cluster address of YT cluster http balancer, examples:
         *                "hahn"
         *                "arnold.yt.yandex.net"
         *                "localhost:8054"
         */
        public TBuilder setCluster(String cluster) {
            setClusters(cluster);
            return self();
        }

        /**
         * Set heavy executor for YT client. This is used for deserialization of lookup/select response.
         * By default, ForkJoinPool.commonPool().
         * @return self
         */
        public TBuilder setHeavyExecutor(Executor heavyExecutor) {
            this.heavyExecutor = heavyExecutor;
            return self();
        }

        /**
         * Create and use default connector with specified working thread count.
         */
        public TBuilder setDefaultBusConnectorWithThreadCount(int threadCount) {
            setOwnBusConnector(new DefaultBusConnector(new NioEventLoopGroup(threadCount), true));
            isBusConnectorOwner = true;
            return self();
        }

        /**
         * Set name of preferred cluster
         *
         * <p>
         * When YT is configured to use multiple clusters and preferred cluster is set
         * it will be used for all requests unless it is unavailable.
         *
         * <p>
         * If preferred cluster is not set or is set but unavailable YtClient chooses
         * cluster based on network metrics.
         */
        public TBuilder setPreferredClusterName(@Nullable String preferredClusterName) {
            this.preferredClusterName = YtCluster.normalizeName(preferredClusterName);
            return self();
        }

        /**
         * Set proxy role to use.
         *
         * <p>
         * Projects that have dedicated proxies should use this option so YtClient will use them.
         * If no proxy role is specified default proxies will be used.
         */
        public TBuilder setProxyRole(@Nullable String proxyRole) {
            this.proxyRole = proxyRole;
            return self();
        }

        void validate() {
            if (!enableValidation) {
                return;
            }

            if (clusters.isEmpty()) {
                throw new IllegalArgumentException("No YT cluster specified");
            }

            // Check cluster uniqueness.
            Set<String> clusterNames = new HashSet<>();
            boolean foundPreferredCluster = false;
            for (YtCluster cluster : clusters) {
                if (clusterNames.contains(cluster.name)) {
                    throw new IllegalArgumentException(
                            String.format("Cluster %s is specified multiple times",
                                    cluster.name));
                }
                clusterNames.add(cluster.name);
                if (cluster.name.equals(preferredClusterName)) {
                    foundPreferredCluster = true;
                }
            }

            if (preferredClusterName != null && !foundPreferredCluster) {
                throw new IllegalArgumentException(
                        String.format("Preferred cluster %s is not found among specified clusters",
                                preferredClusterName));
            }
        }

        TBuilder disableValidation() {
            enableValidation = false;
            return self();
        }
    }

    // Class is able to initialize missing fields of builder with reasonable defaults. Keep in mind that:
    //   1. We cannot initialize this fields in YtClient constructor
    //   because busConnector is required to initialize superclass.
    //   2. We don't want to call initialization code if user specified explicitly values
    //   (initialization code is looking at environment and can fail, and it starts threads)
    //   3. Its better not to touch and modify builder instance since it's theoretically can be used by
    //   user to initialize another YtClient.
    @NonNullFields
    public static class BuilderWithDefaults<
            TClient extends YtClientOpensource,
            TBuilder extends ClientBuilder<TClient, TBuilder>> {
        final ClientBuilder<TClient, TBuilder> builder;
        final BusConnector busConnector;
        final RpcCredentials credentials;

        public BuilderWithDefaults(ClientBuilder<TClient, TBuilder> builder) {
            this.builder = builder;

            if (builder.busConnector != null) {
                busConnector = builder.busConnector;
            } else {
                busConnector = new DefaultBusConnector();
            }

            if (builder.credentials != null) {
                credentials = builder.credentials;
            } else {
                credentials = RpcCredentials.loadFromEnvironment();
            }
        }
    }
}
